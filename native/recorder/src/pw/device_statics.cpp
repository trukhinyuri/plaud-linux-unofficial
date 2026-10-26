// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "pw/device_statics.h"

#include <pipewire/pipewire.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pw/pw_connection.h"

namespace plaud_rec {

namespace {

constexpr int kConnectTimeoutMs = 1500;
constexpr int kSyncTimeoutMs = 1500;
constexpr int kDebounceMs = 150;
constexpr int kReconnectPollMs = 500;

struct MicListPayload {
  std::vector<std::string> names;
};

struct MutePayload {
  bool status = false;
  bool muted = false;
  std::string level;
  std::string message;
};

void DeliverMicList(Napi::Env env, Napi::Function fn, MicListPayload* payload) {
  std::unique_ptr<MicListPayload> owned(payload);
  if (static_cast<napi_env>(env) == nullptr || fn.IsEmpty()) return;
  Napi::Array names = Napi::Array::New(env, owned->names.size());
  for (uint32_t i = 0; i < owned->names.size(); ++i) names.Set(i, owned->names[i]);
  try {
    fn.Call({names});
  } catch (const Napi::Error&) {
    // A throwing listener must not tear down the watcher.
  }
}

void DeliverMuteEvent(Napi::Env env, Napi::Function fn, MutePayload* payload) {
  std::unique_ptr<MutePayload> owned(payload);
  if (static_cast<napi_env>(env) == nullptr || fn.IsEmpty()) return;
  Napi::Object ev = Napi::Object::New(env);
  Napi::Object obj = Napi::Object::New(env);
  if (owned->status) {
    ev.Set("type", "status");
    obj.Set("muted", owned->muted);
  } else {
    ev.Set("type", "log");
    obj.Set("level", owned->level);
    obj.Set("message", owned->message);
  }
  ev.Set("obj", obj);
  try {
    fn.Call({ev});
  } catch (const Napi::Error&) {
  }
}

std::string MicFingerprint(const RegistrySnapshot& snap) {
  std::string fp;
  for (const PwAudioNode* n : snap.Sources()) {
    fp += std::to_string(n->id);
    fp += '\x1f';
    fp += n->name;
    fp += '\x1f';
    fp += n->DisplayName();
    fp += '\x1e';
  }
  const PwAudioNode* def = snap.DefaultSource();
  fp += "default=";
  fp += def != nullptr ? def->name : std::string();
  return fp;
}

std::vector<std::string> MicNames(const RegistrySnapshot& snap) {
  std::vector<std::string> names;
  for (const PwAudioNode* n : snap.Sources()) names.push_back(n->DisplayName());
  return names;
}

// One process-wide PipeWire connection for the long-lived device watchers.
class DeviceHub {
 public:
  static DeviceHub& Get() {
    // Never destroyed: watchers may outlive static destruction order at exit.
    static DeviceHub* hub = new DeviceHub();
    return *hub;
  }

  bool WatchMicrophones(Napi::Env env, const Napi::Function& callback, std::string* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!AcquireLocked(error)) return false;
    Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(env, callback, "MicrophoneDeviceWatcher", 0, 1);
    WithLoopLocked([&] {
      if (micActive_) {
        micTsfn_.Release();
      } else {
        napi_add_env_cleanup_hook(env, &DeviceHub::MicEnvCleanup, this);
        micEnv_ = env;
      }
      micTsfn_ = tsfn;
      micActive_ = true;
      micFingerprint_ = MicFingerprint(registry_->Snapshot());
    });
    return true;
  }

  void StopWatchingMicrophones(bool envTeardown) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!micActive_) return;
      WithLoopLocked([&] {
        micActive_ = false;
        if (envTeardown) {
          micTsfn_.Abort();
        } else {
          micTsfn_.Release();
          napi_remove_env_cleanup_hook(micEnv_, &DeviceHub::MicEnvCleanup, this);
        }
        micTsfn_ = Napi::ThreadSafeFunction();
        micEnv_ = nullptr;
      });
    }
    ReleaseIfIdle();
  }

  // Throws the official error messages.
  void StartMuteMonitor(Napi::Env env, const Napi::Function& callback) {
    std::lock_guard<std::mutex> lock(mu_);
    if (muteActive_) throw Napi::Error::New(env, "Speaker mute monitor already started.");
    std::string error;
    if (!AcquireLocked(&error)) throw Napi::Error::New(env, "Failed to start speaker mute monitor.");
    Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(env, callback, "SpeakerMuteMonitor", 0, 1);
    WithLoopLocked([&] {
      napi_add_env_cleanup_hook(env, &DeviceHub::MuteEnvCleanup, this);
      muteEnv_ = env;
      muteTsfn_ = tsfn;
      muteActive_ = true;
      muteDevice_ = SPA_ID_INVALID;
      EvaluateMuteLocked(registry_->Snapshot(), true);
      EmitMuteLog("info", "LinuxSpeakerMuteMonitor started.");
      EmitMuteStatus(muted_.load());
    });
  }

  void StopMuteMonitor(bool envTeardown) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!muteActive_) return;
      WithLoopLocked([&] {
        if (!envTeardown) {
          if (muteDevice_ != SPA_ID_INVALID) {
            EmitMuteLog("info", "Removed mute listener from device " + std::to_string(muteDevice_));
          }
          EmitMuteLog("info", "LinuxSpeakerMuteMonitor stopped.");
        }
        muteActive_ = false;
        if (envTeardown) {
          muteTsfn_.Abort();
        } else {
          muteTsfn_.Release();
          napi_remove_env_cleanup_hook(muteEnv_, &DeviceHub::MuteEnvCleanup, this);
        }
        muteTsfn_ = Napi::ThreadSafeFunction();
        muteEnv_ = nullptr;
        muteDevice_ = SPA_ID_INVALID;
      });
    }
    ReleaseIfIdle();
  }

  bool MuteMonitorActive() {
    std::lock_guard<std::mutex> lock(mu_);
    return muteActive_;
  }

  bool Muted() const { return muted_.load(); }

  bool SnapshotIfConnected(RegistrySnapshot* out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!conn_ || !registry_ || broken_.load() || !conn_->connected()) return false;
    *out = registry_->Snapshot();
    return true;
  }

 private:
  DeviceHub() = default;

  static void MicEnvCleanup(void* arg) { static_cast<DeviceHub*>(arg)->StopWatchingMicrophones(true); }
  static void MuteEnvCleanup(void* arg) { static_cast<DeviceHub*>(arg)->StopMuteMonitor(true); }

  template <typename F>
  void WithLoopLocked(F&& fn) {
    if (conn_ && conn_->threadLoop() != nullptr) {
      PwLock lock(*conn_);
      fn();
    } else {
      fn();
    }
  }

  // mu_ held.
  bool AcquireLocked(std::string* error) {
    if (conn_ && registry_ && !broken_.load() && conn_->connected()) return true;
    DisconnectLocked();
    if (!ConnectLocked(error)) return false;
    if (!supervisor_.joinable()) {
      const uint64_t gen = ++generation_;
      supervisor_ = std::thread([this, gen] { SupervisorMain(gen); });
    }
    return true;
  }

  // mu_ held.
  bool ConnectLocked(std::string* error) {
    conn_ = std::make_unique<PwConnection>("plaud-devices-pw");
    broken_ = false;
    conn_->SetErrorHandler([this](uint32_t id, int res, const std::string&) {
      if (id == PW_ID_CORE && res == -EPIPE) {
        broken_ = true;
        cv_.notify_all();
      }
    });
    if (!conn_->Connect(kConnectTimeoutMs, error)) {
      conn_.reset();
      return false;
    }
    bool ok = true;
    {
      PwLock lock(*conn_);
      registry_ = std::make_unique<PwRegistry>(conn_.get());
      if (!registry_->Start([this](uint32_t mask) { OnGraphChanged(mask); })) {
        *error = "cannot get the PipeWire registry";
        ok = false;
      } else {
        timer_ = pw_loop_add_timer(conn_->loop(), &DeviceHub::OnDebounceTimer, this);
        if (!conn_->SyncLocked(kSyncTimeoutMs) || !conn_->SyncLocked(kSyncTimeoutMs)) {
          *error = "PipeWire did not answer the registry round trip";
          ok = false;
        }
      }
    }
    if (!ok) DisconnectLocked();
    return ok;
  }

  // mu_ held.
  void DisconnectLocked() {
    if (!conn_) return;
    if (conn_->threadLoop() != nullptr) {
      PwLock lock(*conn_);
      if (timer_ != nullptr) {
        pw_loop_destroy_source(conn_->loop(), timer_);
        timer_ = nullptr;
      }
      if (registry_) registry_->Stop();
    }
    registry_.reset();
    conn_->Disconnect();
    conn_.reset();
  }

  void ReleaseIfIdle() {
    std::thread finished;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (micActive_ || muteActive_) return;
      DisconnectLocked();
      ++generation_;
      finished = std::move(supervisor_);
    }
    cv_.notify_all();
    if (finished.joinable()) finished.join();
  }

  // Reconnects after the PipeWire daemon went away (EPIPE) while watchers
  // are active, then re-announces the current state to them.
  void SupervisorMain(uint64_t gen) {
    std::unique_lock<std::mutex> lock(mu_);
    while (gen == generation_) {
      cv_.wait_for(lock, std::chrono::milliseconds(kReconnectPollMs));
      if (gen != generation_) break;
      if (!(micActive_ || muteActive_)) continue;
      if (conn_ && !broken_.load()) continue;
      DisconnectLocked();
      std::string error;
      if (!ConnectLocked(&error)) continue;
      WithLoopLocked([&] {
        const RegistrySnapshot snap = registry_->Snapshot();
        if (micActive_) {
          std::string fp = MicFingerprint(snap);
          if (fp != micFingerprint_) {
            micFingerprint_ = std::move(fp);
            EmitMicList(snap);
          }
        }
        if (muteActive_) {
          EmitMuteLog("info", "PipeWire connection re-established.");
          EvaluateMuteLocked(snap, false);
        }
      });
    }
  }

  // Loop thread, loop lock held.
  void OnGraphChanged(uint32_t mask) {
    if (micActive_ && (mask & (PwRegistry::kNodes | PwRegistry::kDefaults)) != 0 && timer_ != nullptr) {
      timespec value{};
      value.tv_sec = 0;
      value.tv_nsec = kDebounceMs * 1000000L;
      pw_loop_update_timer(conn_->loop(), timer_, &value, nullptr, false);
    }
    if (muteActive_ && (mask & (PwRegistry::kNodes | PwRegistry::kDefaults | PwRegistry::kProps)) != 0) {
      EvaluateMuteLocked(registry_->Snapshot(), false);
    }
  }

  static void OnDebounceTimer(void* data, uint64_t expirations) {
    (void)expirations;
    auto* self = static_cast<DeviceHub*>(data);
    if (!self->micActive_ || !self->registry_) return;
    const RegistrySnapshot snap = self->registry_->Snapshot();
    std::string fp = MicFingerprint(snap);
    if (fp == self->micFingerprint_) return;
    self->micFingerprint_ = std::move(fp);
    self->EmitMicList(snap);
  }

  void EmitMicList(const RegistrySnapshot& snap) {
    if (!micActive_) return;
    auto* payload = new MicListPayload{MicNames(snap)};
    if (micTsfn_.NonBlockingCall(payload, DeliverMicList) != napi_ok) delete payload;
  }

  void EmitMuteLog(const char* level, const std::string& message) {
    if (!muteActive_) return;
    auto* payload = new MutePayload();
    payload->level = level;
    payload->message = message;
    if (muteTsfn_.NonBlockingCall(payload, DeliverMuteEvent) != napi_ok) delete payload;
  }

  void EmitMuteStatus(bool muted) {
    if (!muteActive_) return;
    auto* payload = new MutePayload();
    payload->status = true;
    payload->muted = muted;
    if (muteTsfn_.NonBlockingCall(payload, DeliverMuteEvent) != napi_ok) delete payload;
  }

  // Follows the default output's mute flag (the official monitor reads the
  // device mute property only, not the volume).
  void EvaluateMuteLocked(const RegistrySnapshot& snap, bool starting) {
    const PwAudioNode* sink = snap.DefaultSink();
    const uint32_t device = sink != nullptr ? sink->id : SPA_ID_INVALID;
    if (device != muteDevice_) {
      if (!starting) {
        if (muteDevice_ != SPA_ID_INVALID) {
          EmitMuteLog("info", "Removed mute listener from device " + std::to_string(muteDevice_));
        }
        EmitMuteLog("info", "Default output device changed.");
      }
      muteDevice_ = device;
      if (sink != nullptr) {
        if (sink->propsKnown) {
          EmitMuteLog("info", "Added mute listener to device " + std::to_string(device));
        } else {
          EmitMuteLog("warning", "Device " + std::to_string(device) + " does not expose mute property.");
        }
      }
    }
    const bool muted = sink != nullptr && sink->propsKnown && sink->mute;
    if (muted != muted_.load()) {
      muted_ = muted;
      if (!starting) {
        EmitMuteLog("info", "Mute state changed on device " + std::to_string(device) + ": " +
                                (muted ? "muted" : "unmuted"));
        EmitMuteStatus(muted);
      }
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::thread supervisor_;
  uint64_t generation_ = 0;
  std::atomic<bool> broken_{false};

  std::unique_ptr<PwConnection> conn_;
  std::unique_ptr<PwRegistry> registry_;
  spa_source* timer_ = nullptr;

  bool micActive_ = false;
  napi_env micEnv_ = nullptr;
  Napi::ThreadSafeFunction micTsfn_;
  std::string micFingerprint_;

  bool muteActive_ = false;
  napi_env muteEnv_ = nullptr;
  Napi::ThreadSafeFunction muteTsfn_;
  uint32_t muteDevice_ = SPA_ID_INVALID;
  std::atomic<bool> muted_{false};
};

Napi::Array DeviceNames(Napi::Env env, const std::vector<const PwAudioNode*>& nodes) {
  Napi::Array out = Napi::Array::New(env, nodes.size());
  for (uint32_t i = 0; i < nodes.size(); ++i) out.Set(i, nodes[i]->DisplayName());
  return out;
}

Napi::Array DeviceDetails(Napi::Env env, const std::vector<const PwAudioNode*>& nodes, const PwAudioNode* def) {
  Napi::Array out = Napi::Array::New(env, nodes.size());
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    Napi::Object o = Napi::Object::New(env);
    o.Set("id", Napi::Number::New(env, nodes[i]->id));
    o.Set("name", nodes[i]->DisplayName());
    o.Set("uid", nodes[i]->name);
    o.Set("isDefault", def != nullptr && def->id == nodes[i]->id);
    out.Set(i, o);
  }
  return out;
}

// Logs either to an optional JS callback ({level, message}) or nowhere.
class PermissionLogger {
 public:
  PermissionLogger(Napi::Env env, const Napi::CallbackInfo& info) : env_(env) {
    if (info.Length() > 0 && info[0].IsFunction()) fn_ = info[0].As<Napi::Function>();
  }
  void operator()(const char* level, const std::string& message) const {
    if (fn_.IsEmpty()) return;
    Napi::Object o = Napi::Object::New(env_);
    o.Set("level", level);
    o.Set("message", message);
    try {
      fn_.Call({o});
    } catch (const Napi::Error&) {
    }
  }

 private:
  Napi::Env env_;
  Napi::Function fn_;
};

bool ProbePipeWire(std::string* why) {
  RegistrySnapshot ignored;
  if (DeviceHub::Get().SnapshotIfConnected(&ignored)) return true;
  PwConnection conn("plaud-probe-pw");
  if (!conn.Connect(kConnectTimeoutMs, why)) return false;
  conn.Disconnect();
  return true;
}

}  // namespace

bool TakeDeviceSnapshot(RegistrySnapshot* out, std::string* error) {
  if (DeviceHub::Get().SnapshotIfConnected(out)) return true;
  PwConnection conn("plaud-query-pw");
  if (!conn.Connect(kConnectTimeoutMs, error)) return false;
  bool ok = true;
  {
    PwLock lock(conn);
    PwRegistry registry(&conn);
    if (!registry.Start(nullptr)) {
      *error = "cannot get the PipeWire registry";
      ok = false;
    } else if (!conn.SyncLocked(kSyncTimeoutMs) || !conn.SyncLocked(kSyncTimeoutMs)) {
      *error = "PipeWire did not answer the registry round trip";
      ok = false;
    } else {
      *out = registry.Snapshot();
    }
    registry.Stop();
  }
  conn.Disconnect();
  return ok;
}

Napi::Value CheckSystemAudioPermission(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  PermissionLogger log(env, info);
  log("info", "Starting system audio permission check");
  std::string why;
  if (ProbePipeWire(&why)) {
    log("info", "System audio permission status: Granted");
    return Napi::Number::New(env, 0);
  }
  log("error", "PipeWire is not reachable: " + why);
  log("info", "System audio permission status: Denied");
  return Napi::Number::New(env, 1);
}

Napi::Value RequestSystemAudioPermission(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  PermissionLogger log(env, info);
  log("info", "Starting system audio permission request");
  std::string why;
  const bool granted = ProbePipeWire(&why);
  if (!granted) log("error", "PipeWire is not reachable: " + why);
  log("info", std::string("System audio permission request result: ") + (granted ? "granted" : "denied"));
  return Napi::Boolean::New(env, granted);
}

Napi::Value ListMicrophoneDevices(const Napi::CallbackInfo& info) {
  RegistrySnapshot snap;
  std::string error;
  if (!TakeDeviceSnapshot(&snap, &error)) return Napi::Array::New(info.Env());
  return DeviceNames(info.Env(), snap.Sources());
}

Napi::Value ListMicrophoneDevicesDetailed(const Napi::CallbackInfo& info) {
  RegistrySnapshot snap;
  std::string error;
  if (!TakeDeviceSnapshot(&snap, &error)) return Napi::Array::New(info.Env());
  return DeviceDetails(info.Env(), snap.Sources(), snap.DefaultSource());
}

Napi::Value ListSpeakerDevices(const Napi::CallbackInfo& info) {
  RegistrySnapshot snap;
  std::string error;
  if (!TakeDeviceSnapshot(&snap, &error)) return Napi::Array::New(info.Env());
  return DeviceNames(info.Env(), snap.Sinks());
}

Napi::Value ListSpeakerDevicesDetailed(const Napi::CallbackInfo& info) {
  RegistrySnapshot snap;
  std::string error;
  if (!TakeDeviceSnapshot(&snap, &error)) return Napi::Array::New(info.Env());
  return DeviceDetails(info.Env(), snap.Sinks(), snap.DefaultSink());
}

Napi::Value WatchMicrophoneDevices(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsFunction()) {
    throw Napi::TypeError::New(env, "watchMicrophoneDevices requires a callback");
  }
  std::string error;
  if (!DeviceHub::Get().WatchMicrophones(env, info[0].As<Napi::Function>(), &error)) {
    throw Napi::Error::New(env, "Failed to register microphone device listener (status -1): " + error);
  }
  return Napi::Boolean::New(env, true);
}

Napi::Value StopWatchingMicrophoneDevices(const Napi::CallbackInfo& info) {
  DeviceHub::Get().StopWatchingMicrophones(false);
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value StartSpeakerMuteMonitor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsFunction()) {
    throw Napi::TypeError::New(env, "startSpeakerMuteMonitor requires a callback function.");
  }
  DeviceHub::Get().StartMuteMonitor(env, info[0].As<Napi::Function>());
  return env.Undefined();
}

Napi::Value StopSpeakerMuteMonitor(const Napi::CallbackInfo& info) {
  DeviceHub::Get().StopMuteMonitor(false);
  return info.Env().Undefined();
}

Napi::Value IsSpeakerMuted(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  DeviceHub& hub = DeviceHub::Get();
  if (!hub.MuteMonitorActive()) throw Napi::Error::New(env, "Speaker mute monitor is not started.");
  return Napi::Boolean::New(env, hub.Muted());
}

}  // namespace plaud_rec
