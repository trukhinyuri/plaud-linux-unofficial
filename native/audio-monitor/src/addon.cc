// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// N-API surface of audio_monitor_linux.node, identical to the official
// audio_monitor_mac.node / audio_monitor_win.node:
//
//   const { AudioMonitor } = require("audio_monitor_linux.node");
//   const m = new AudioMonitor();          // no thread, no PipeWire connection yet
//   m.start(cb);                           // meeting detection; cb({type:"log"|"monitor", obj})
//   m.stop();
//   m.startMicMonitor(cb) -> initialUID;   // cb({type:"micEvent", micEventType, deviceId, deviceName})
//   m.stopMicMonitor();
//   m.startSpeakerMonitor(cb) -> initialUID; // cb({type:"speakerEvent", speakerEventType, deviceId, deviceName})
//   m.stopSpeakerMonitor();
//
// All native threads deliver through ThreadSafeFunctions (never blocking);
// logs of every component go to the start() callback, nothing is printed.
#include <napi.h>
#include <unistd.h>

#include <memory>
#include <mutex>

#include "meeting_detector.h"
#include "mic_monitor.h"
#include "pw_engine.h"
#include "speaker_monitor.h"

namespace {

using am::Props;

struct Payload {
  enum class Kind { Log, Monitor, Mic, Speaker };
  Kind kind;
  std::string a;  // level | appName | eventType
  std::string b;  // message | deviceId
  std::string c;  // deviceName
  bool active = false;
  Props meta;
};

Napi::Object propsToObject(Napi::Env env, const Props& props) {
  Napi::Object o = Napi::Object::New(env);
  for (const auto& kv : props) o.Set(kv.first, Napi::String::New(env, kv.second));
  return o;
}

void callJs(Napi::Env env, Napi::Function callback, Payload* payload) {
  std::unique_ptr<Payload> p(payload);
  if (env == nullptr || callback.IsEmpty()) return;
  Napi::Object event = Napi::Object::New(env);
  switch (p->kind) {
    case Payload::Kind::Log: {
      Napi::Object obj = Napi::Object::New(env);
      obj.Set("source", "NODE");
      obj.Set("level", p->a);
      obj.Set("message", p->b);
      obj.Set("meta", propsToObject(env, p->meta));
      event.Set("type", "log");
      event.Set("obj", obj);
      break;
    }
    case Payload::Kind::Monitor: {
      Napi::Object obj = Napi::Object::New(env);
      obj.Set("notifyType", Napi::Number::New(env, 1));
      obj.Set("appName", p->a);
      obj.Set("isActive", Napi::Boolean::New(env, p->active));
      obj.Set("meta", propsToObject(env, p->meta));
      event.Set("type", "monitor");
      event.Set("obj", obj);
      break;
    }
    case Payload::Kind::Mic:
      event.Set("type", "micEvent");
      event.Set("micEventType", p->a);
      event.Set("deviceId", p->b);
      event.Set("deviceName", p->c);
      break;
    case Payload::Kind::Speaker:
      event.Set("type", "speakerEvent");
      event.Set("speakerEventType", p->a);
      event.Set("deviceId", p->b);
      event.Set("deviceName", p->c);
      break;
  }
  callback.Call({event});
}

// A ThreadSafeFunction that native threads may post to at any time; posts
// after release (or after Node finalized the function) are dropped.
class Channel : public std::enable_shared_from_this<Channel> {
 public:
  void open(Napi::Env env, Napi::Function callback, const char* resourceName) {
    close();
    auto* keepAlive = new std::shared_ptr<Channel>(shared_from_this());
    Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(
        env, callback, resourceName, 0, 1, keepAlive, [](Napi::Env, std::shared_ptr<Channel>* self) {
          {
            std::lock_guard<std::mutex> lock((*self)->mu_);
            (*self)->finalized_ += 1;
            if ((*self)->finalized_ == (*self)->generation_) (*self)->active_ = false;
          }
          delete self;
        });
    std::lock_guard<std::mutex> lock(mu_);
    tsfn_ = tsfn;
    active_ = true;
    generation_ += 1;
  }

  void close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) return;
    active_ = false;
    tsfn_.Release();
  }

  bool active() {
    std::lock_guard<std::mutex> lock(mu_);
    return active_;
  }

  void post(std::unique_ptr<Payload> payload) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) return;
    Payload* raw = payload.release();
    if (tsfn_.NonBlockingCall(raw, callJs) != napi_ok) delete raw;
  }

 private:
  std::mutex mu_;
  Napi::ThreadSafeFunction tsfn_;
  bool active_ = false;
  uint64_t generation_ = 0;
  uint64_t finalized_ = 0;
};

class DeviceSink : public am::DeviceEventSink {
 public:
  DeviceSink(std::shared_ptr<Channel> channel, Payload::Kind kind) : channel_(std::move(channel)), kind_(kind) {}
  void deviceEvent(const std::string& type, const std::string& id, const std::string& name) override {
    std::unique_ptr<Payload> p(new Payload{kind_, type, id, name, false, {}});
    channel_->post(std::move(p));
  }

 private:
  std::shared_ptr<Channel> channel_;
  Payload::Kind kind_;
};

class AudioMonitor : public Napi::ObjectWrap<AudioMonitor>, public am::MonitorSink, public am::GraphObserver {
 public:
  static Napi::Function Init(Napi::Env env) {
    return DefineClass(env, "AudioMonitor",
                       {
                           InstanceMethod<&AudioMonitor::Start>("start"),
                           InstanceMethod<&AudioMonitor::Stop>("stop"),
                           InstanceMethod<&AudioMonitor::StartMicMonitor>("startMicMonitor"),
                           InstanceMethod<&AudioMonitor::StopMicMonitor>("stopMicMonitor"),
                           InstanceMethod<&AudioMonitor::StartSpeakerMonitor>("startSpeakerMonitor"),
                           InstanceMethod<&AudioMonitor::StopSpeakerMonitor>("stopSpeakerMonitor"),
                       });
  }

  explicit AudioMonitor(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<AudioMonitor>(info),
        env_(info.Env()),
        mainChannel_(std::make_shared<Channel>()),
        micChannel_(std::make_shared<Channel>()),
        speakerChannel_(std::make_shared<Channel>()),
        micSink_(micChannel_, Payload::Kind::Mic),
        speakerSink_(speakerChannel_, Payload::Kind::Speaker) {
    std::weak_ptr<Channel> logChannel = mainChannel_;
    engine_.reset(new am::PwEngine([logChannel](const std::string& level, const std::string& message, const Props& meta) {
      if (auto ch = logChannel.lock()) ch->post(std::unique_ptr<Payload>(new Payload{Payload::Kind::Log, level, message, "", false, meta}));
    }));
    mic_.reset(new am::MicMonitor(*engine_, micSink_));
    speaker_.reset(new am::SpeakerMonitor(*engine_, speakerSink_));
    napi_add_env_cleanup_hook(env_, &AudioMonitor::CleanupHook, this);
  }

  ~AudioMonitor() override {
    if (!cleanupRan_) napi_remove_env_cleanup_hook(env_, &AudioMonitor::CleanupHook, this);
    Shutdown();
  }

  // am::MonitorSink (detector thread)
  void log(const std::string& level, const std::string& message, const Props& meta) override {
    mainChannel_->post(std::unique_ptr<Payload>(new Payload{Payload::Kind::Log, level, message, "", false, meta}));
  }
  void monitor(const std::string& appName, bool isActive, const Props& meta) override {
    mainChannel_->post(std::unique_ptr<Payload>(new Payload{Payload::Kind::Monitor, appName, "", "", isActive, meta}));
  }

  // am::GraphObserver (PipeWire loop thread, lock held): feeds the detector.
  void onGraphChanged() override {
    if (detector_) detector_->post(engine_->captureSnapshot());
  }
  void onConnectionLost() override {
    if (detector_) detector_->post(am::CaptureSnapshot());
  }

 private:
  static void CleanupHook(void* arg) {
    auto* self = static_cast<AudioMonitor*>(arg);
    self->cleanupRan_ = true;
    self->Shutdown();
  }

  void Shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    StopMeeting(false);
    mic_->stop();
    speaker_->stop();
    micChannel_->close();
    speakerChannel_->close();
    mainChannel_->close();
  }

  void StopMeeting(bool logStop) {
    if (!meetingRunning_) return;
    if (logStop) log("info", "Stopping monitor", {});
    {
      am::PwLock lock(*engine_);
      engine_->removeObserver(this);
    }
    engine_->removeUser();
    detector_->stop();
    detector_.reset();
    meetingRunning_ = false;
    mainChannel_->close();
  }

  Napi::Value Start(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (meetingRunning_) {
      Napi::Error::New(env, "Monitor has already been started.").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (info.Length() < 1 || !info[0].IsFunction()) {
      Napi::TypeError::New(env, "A callback function is required.").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    shutdown_ = false;
    mainChannel_->open(env, info[0].As<Napi::Function>(), "AudioMonitorCallback");
    log("info", "Starting PipeWire monitor v" AUDIO_MONITOR_VERSION, {{"pid", std::to_string(::getpid())}});
    detector_.reset(new am::MeetingDetector(*this, static_cast<int>(::getpid())));
    detector_->start();
    engine_->addUser();
    {
      am::PwLock lock(*engine_);
      engine_->addObserver(this);
      if (engine_->synced()) onGraphChanged();
    }
    meetingRunning_ = true;
    return env.Undefined();
  }

  Napi::Value Stop(const Napi::CallbackInfo& info) {
    StopMeeting(true);
    return info.Env().Undefined();
  }

  Napi::Value StartMicMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsFunction()) {
      Napi::TypeError::New(env, "A callback function is required.").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    shutdown_ = false;
    micChannel_->open(env, info[0].As<Napi::Function>(), "MicMonitorCallback");
    return Napi::String::New(env, mic_->running() ? mic_->currentUid() : mic_->start());
  }

  Napi::Value StopMicMonitor(const Napi::CallbackInfo& info) {
    mic_->stop();
    micChannel_->close();
    return info.Env().Undefined();
  }

  Napi::Value StartSpeakerMonitor(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsFunction()) {
      Napi::TypeError::New(env, "A callback function is required.").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    shutdown_ = false;
    speakerChannel_->open(env, info[0].As<Napi::Function>(), "SpeakerMonitorCallback");
    return Napi::String::New(env, speaker_->running() ? speaker_->currentUid() : speaker_->start());
  }

  Napi::Value StopSpeakerMonitor(const Napi::CallbackInfo& info) {
    speaker_->stop();
    speakerChannel_->close();
    return info.Env().Undefined();
  }

  napi_env env_;
  std::shared_ptr<Channel> mainChannel_;
  std::shared_ptr<Channel> micChannel_;
  std::shared_ptr<Channel> speakerChannel_;
  DeviceSink micSink_;
  DeviceSink speakerSink_;
  std::unique_ptr<am::PwEngine> engine_;
  std::unique_ptr<am::MeetingDetector> detector_;
  std::unique_ptr<am::MicMonitor> mic_;
  std::unique_ptr<am::SpeakerMonitor> speaker_;
  bool meetingRunning_ = false;
  bool cleanupRan_ = false;
  bool shutdown_ = false;
};

Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
  exports.Set("AudioMonitor", AudioMonitor::Init(env));
  return exports;
}

}  // namespace

NODE_API_MODULE(audio_monitor_linux, InitModule)
