// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "recorder_wrapper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

#include "aec_metrics/aec_metrics_napi.h"
#include "pw/device_statics.h"
#include "screen/screen_napi.h"

namespace plaud_rec {

namespace {

constexpr double kMaxFillSilenceMs = 24.0 * 3600.0 * 1000.0;

enum class PauseMode { Microphone, SystemAudio, Both };

// begin()/pause(): no argument means both sources; otherwise the low byte of
// the uint32 value selects 0 = microphone, 1 = system audio, else both.
PauseMode ParsePauseMode(const Napi::CallbackInfo& info, const char* error) {
  if (info.Length() == 0) return PauseMode::Both;
  if (!info[0].IsNumber()) throw Napi::TypeError::New(info.Env(), error);
  const uint8_t mode = static_cast<uint8_t>(info[0].As<Napi::Number>().Uint32Value() & 0xffu);
  if (mode == 0) return PauseMode::Microphone;
  if (mode == 1) return PauseMode::SystemAudio;
  return PauseMode::Both;
}

// setAudioDevice numbers: "<label> must be within uint32 range." /
// "<label> must be an integer."; start() options use the "non-negative
// integer" wording for the range error.
uint32_t ParseUint32(const Napi::Env& env, const Napi::Value& v, const std::string& label, const char* rangeSuffix) {
  const double d = v.As<Napi::Number>().DoubleValue();
  if (!(d >= 0.0 && d <= 4294967295.0)) throw Napi::TypeError::New(env, label + rangeSuffix);
  if (d != std::floor(d)) throw Napi::TypeError::New(env, label + " must be an integer.");
  return static_cast<uint32_t>(d);
}

constexpr const char* kSetDeviceRange = " must be within uint32 range.";
constexpr const char* kStartRange = " must be a non-negative integer within uint32 range.";

DeviceSelection SelectId(uint32_t id) {
  DeviceSelection s;
  s.kind = DeviceSelection::Kind::Id;
  s.id = id;
  return s;
}

DeviceSelection SelectUid(std::string uid) {
  DeviceSelection s;
  s.kind = DeviceSelection::Kind::Uid;
  s.uid = std::move(uid);
  return s;
}

// A device reference: number (id), string (uid), {deviceId}|{deviceUid}|
// {uid}|{id}, or null/undefined for the system default.
DeviceSelection ParseDeviceValue(const Napi::Env& env, const Napi::Value& v, const std::string& label,
                                 const char* rangeSuffix) {
  if (v.IsUndefined() || v.IsNull()) return DeviceSelection{};
  if (v.IsNumber()) return SelectId(ParseUint32(env, v, label, rangeSuffix));
  if (v.IsString()) return SelectUid(v.As<Napi::String>().Utf8Value());
  if (v.IsObject()) {
    Napi::Object o = v.As<Napi::Object>();
    if (o.Has("deviceId") && o.Get("deviceId").IsNumber()) {
      return SelectId(ParseUint32(env, o.Get("deviceId"), label + ".deviceId", rangeSuffix));
    }
    if (o.Has("deviceUid") && o.Get("deviceUid").IsString()) {
      return SelectUid(o.Get("deviceUid").As<Napi::String>().Utf8Value());
    }
    if (o.Has("uid") && o.Get("uid").IsString()) return SelectUid(o.Get("uid").As<Napi::String>().Utf8Value());
    if (o.Has("id") && o.Get("id").IsNumber()) return SelectId(ParseUint32(env, o.Get("id"), label + ".id", rangeSuffix));
    throw Napi::TypeError::New(env, label + " object must include deviceId (number) or deviceUid (string).");
  }
  throw Napi::TypeError::New(env, label + " must be a number, string, object, null, or undefined.");
}

// setAudioDevice() result value: null (default), the id, or the uid resolved
// to its id when the device exists (the unresolved string otherwise).
Napi::Value SelectionResult(const Napi::Env& env, const DeviceSelection& sel, const RegistrySnapshot* snap,
                            bool sink) {
  switch (sel.kind) {
    case DeviceSelection::Kind::Default:
      return env.Null();
    case DeviceSelection::Kind::Id:
      return Napi::Number::New(env, sel.id);
    case DeviceSelection::Kind::Uid:
      if (snap != nullptr) {
        if (const PwAudioNode* n = ResolveDeviceSelection(*snap, sel, sink)) return Napi::Number::New(env, n->id);
      }
      return Napi::String::New(env, sel.uid);
  }
  return env.Null();
}

bool OptBool(const Napi::Object& o, const char* key, bool fallback) {
  Napi::Value v = o.Get(key);
  return v.IsBoolean() ? v.As<Napi::Boolean>().Value() : fallback;
}

double OptNumber(const Napi::Object& o, const char* key, double fallback) {
  Napi::Value v = o.Get(key);
  if (!v.IsNumber()) return fallback;
  const double d = v.As<Napi::Number>().DoubleValue();
  return std::isfinite(d) ? d : fallback;
}

std::string Lowercase(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string EnvOrEmpty(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::string(v) : std::string();
}

}  // namespace

Napi::Function RecorderWrapper::Init(Napi::Env env) {
  return DefineClass(
      env, "Recorder",
      {
          InstanceMethod<&RecorderWrapper::Start>("start"),
          InstanceMethod<&RecorderWrapper::Begin>("begin"),
          InstanceMethod<&RecorderWrapper::Pause>("pause"),
          InstanceMethod<&RecorderWrapper::Stop>("stop"),
          InstanceMethod<&RecorderWrapper::Restart>("restart"),
          InstanceMethod<&RecorderWrapper::SetAudioDevice>("setAudioDevice"),
          InstanceMethod<&RecorderWrapper::SetApmEnabled>("setApmEnabled"),
          InstanceMethod<&RecorderWrapper::FillSilence>("fillSilence"),
          InstanceMethod<&RecorderWrapper::SetEncodingPaused>("setEncodingPaused"),
          StaticMethod<&CheckSystemAudioPermission>("checkSystemAudioPermission"),
          StaticMethod<&RequestSystemAudioPermission>("requestSystemAudioPermission"),
          StaticMethod<&CheckScreenCapturePermission>("checkScreenCapturePermission"),
          StaticMethod<&CheckScreenCapturePermissionFresh>("checkScreenCapturePermissionFresh"),
          StaticMethod<&RequestScreenCapturePermission>("requestScreenCapturePermission"),
          StaticMethod<&CaptureScreenImage>("captureScreenImage"),
          StaticMethod<&GetWindowAtCursor>("getWindowAtCursor"),
          StaticMethod<&ExtractTextFromImage>("extractTextFromImage"),
          StaticMethod<&ListMicrophoneDevices>("listMicrophoneDevices"),
          StaticMethod<&ListMicrophoneDevicesDetailed>("listMicrophoneDevicesDetailed"),
          StaticMethod<&WatchMicrophoneDevices>("watchMicrophoneDevices"),
          StaticMethod<&StopWatchingMicrophoneDevices>("stopWatchingMicrophoneDevices"),
          StaticMethod<&ListSpeakerDevices>("listSpeakerDevices"),
          StaticMethod<&ListSpeakerDevicesDetailed>("listSpeakerDevicesDetailed"),
          StaticMethod<&ComputeAecLightMetrics>("computeAecLightMetrics"),
          StaticMethod<&StartSpeakerMuteMonitor>("startSpeakerMuteMonitor"),
          StaticMethod<&StopSpeakerMuteMonitor>("stopSpeakerMuteMonitor"),
          StaticMethod<&IsSpeakerMuted>("isSpeakerMuted"),
      });
}

RecorderWrapper::RecorderWrapper(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<RecorderWrapper>(info), env_(info.Env()) {}

RecorderWrapper::~RecorderWrapper() {
  // Garbage-collected while recording: no JS may run here.
  AbandonRecording();
}

Napi::Value RecorderWrapper::Start(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (running_) throw Napi::Error::New(env, "Recorder is already running. Please stop it first.");
  if (info.Length() < 2 || !info[0].IsObject() || !info[1].IsFunction()) {
    throw Napi::TypeError::New(env, "Start requires (options, callback).");
  }
  Napi::Object opts = info[0].As<Napi::Object>();
  EngineConfig cfg;
  cfg.enableMicrophone = OptBool(opts, "enableMicrophone", false);
  cfg.enableSystemAudio = OptBool(opts, "enableSystemAudio", false);
  cfg.microphoneVolume = OptNumber(opts, "microphoneVolume", 1.0);
  cfg.systemVolume = OptNumber(opts, "systemVolume", 1.0);
  cfg.encoding = OptBool(opts, "encoding", false);
  {
    Napi::Value v = opts.Get("encodingBitRate");
    cfg.encodingBitRate = v.IsNumber() ? v.As<Napi::Number>().Uint32Value() : 32000u;
  }
  cfg.emitApmSignals = OptBool(opts, "emitApmSignals", false);
  cfg.debugDumpRawAudio = OptBool(opts, "debugDumpRawAudio", false);

  // Audio processing backend: options.audioProcessing.backend when
  // audioProcessing is an object, else options.audioProcessingBackend.
  std::string backend = "webrtc";
  std::string aecModel, ansModel;
  Napi::Value ap = opts.Get("audioProcessing");
  if (ap.IsObject()) {
    Napi::Object apo = ap.As<Napi::Object>();
    Napi::Value b = apo.Get("backend");
    if (b.IsString()) backend = b.As<Napi::String>().Utf8Value();
    Napi::Value aec = apo.Get("plaudAecModelPath");
    if (aec.IsString()) aecModel = aec.As<Napi::String>().Utf8Value();
    Napi::Value ans = apo.Get("plaudAnsModelPath");
    if (ans.IsString()) ansModel = ans.As<Napi::String>().Utf8Value();
    if (OptBool(apo, "debugDumpRawAudio", false)) cfg.debugDumpRawAudio = true;
  } else {
    Napi::Value b = opts.Get("audioProcessingBackend");
    if (b.IsString()) backend = b.As<Napi::String>().Utf8Value();
  }
  if (aecModel.empty()) aecModel = EnvOrEmpty("PLAUD_AEC_MODEL_PATH");
  if (ansModel.empty()) ansModel = EnvOrEmpty("PLAUD_ANS_MODEL_PATH");
  const std::string backendKey = Lowercase(backend);
  bool plaud3a = false;
  if (backendKey == "none" || backendKey == "off" || backendKey == "disabled") {
    cfg.backend = ApmBackend::None;
  } else if (backendKey == "webrtc" || backendKey == "apm") {
    cfg.backend = ApmBackend::WebRtc;
  } else if (backendKey == "plaud3a" || backendKey == "plaud_3a" || backendKey == "plaud-3a") {
    plaud3a = true;
  } else {
    throw Napi::TypeError::New(env, "Unknown audioProcessing.backend: " + backend);
  }

  // Device selection: audioDevices.{microphone,speaker}; the legacy flat keys
  // apply only to a side audioDevices leaves unspecified.
  bool micSet = false, spkSet = false;
  Napi::Value devs = opts.Get("audioDevices");
  if (devs.IsObject()) {
    Napi::Object d = devs.As<Napi::Object>();
    if (d.Has("microphone")) {
      cfg.microphone = ParseDeviceValue(env, d.Get("microphone"), "audioDevices.microphone", kStartRange);
      micSet = cfg.microphone.kind != DeviceSelection::Kind::Default;
    }
    if (d.Has("speaker")) {
      cfg.speaker = ParseDeviceValue(env, d.Get("speaker"), "audioDevices.speaker", kStartRange);
      spkSet = cfg.speaker.kind != DeviceSelection::Kind::Default;
    }
  }
  if (plaud3a && (aecModel.empty() || ansModel.empty())) {
    throw Napi::TypeError::New(env,
                               "Plaud 3A selected but model paths are missing. Provide "
                               "options.audioProcessing.plaudAecModelPath + plaudAnsModelPath (or env "
                               "PLAUD_AEC_MODEL_PATH/PLAUD_ANS_MODEL_PATH).");
  }
  auto legacyId = [&](const char* key, DeviceSelection* target, bool* set) {
    Napi::Value v = opts.Get(key);
    if (!v.IsNumber()) return;
    const uint32_t id = ParseUint32(env, v, key, kStartRange);
    if (!*set) {
      *target = SelectId(id);
      *set = true;
    }
  };
  auto legacyUid = [&](const char* key, DeviceSelection* target, bool* set) {
    Napi::Value v = opts.Get(key);
    if (!v.IsString()) return;
    if (!*set) {
      *target = SelectUid(v.As<Napi::String>().Utf8Value());
      *set = true;
    }
  };
  legacyId("audioDeviceId", &cfg.microphone, &micSet);
  legacyId("speakerDeviceId", &cfg.speaker, &spkSet);
  legacyUid("audioDeviceUid", &cfg.microphone, &micSet);
  legacyUid("speakerDeviceUid", &cfg.speaker, &spkSet);
  legacyId("microphoneDeviceId", &cfg.microphone, &micSet);
  if (plaud3a) {
    throw Napi::Error::New(env, "Plaud 3A backend is not compiled in. Rebuild with PC_RECORDER_ENABLE_PLAUD_3A=ON.");
  }
  legacyUid("microphoneDeviceUid", &cfg.microphone, &micSet);

  if (!cfg.enableMicrophone && !cfg.enableSystemAudio) {
    throw Napi::TypeError::New(env, "At least one of enableMicrophone or enableSystemAudio must be true.");
  }

  pump_ = EventPump::Create(env, info[1].As<Napi::Function>());
  engine_ = std::make_unique<CaptureEngine>(pump_);
  std::string tag, detail;
  if (!engine_->Start(cfg, &tag, &detail)) {
    engine_.reset();
    std::shared_ptr<EventPump> pump = std::move(pump_);
    pump->DropFatal();
    pump->Close(env);
    throw Napi::Error::New(env, "Recorder.start failed: Failed to start recorder. [" + tag + "] " + detail);
  }
  running_ = true;
  engine_->SetEncodingPaused(encodingPaused_);
  napi_add_env_cleanup_hook(env, &RecorderWrapper::EnvCleanup, this);
  cleanupHookAdded_ = true;

  Napi::Object result = Napi::Object::New(env);
  result.Set("sampleRate", Napi::Number::New(env, CaptureEngine::kSampleRate));
  result.Set("channels", Napi::Number::New(env, CaptureEngine::kChannels));
  result.Set("container", engine_->encoderActive() ? "ogg" : "pcm");
  return result;
}

Napi::Value RecorderWrapper::Begin(const Napi::CallbackInfo& info) {
  const PauseMode mode = ParsePauseMode(info, "begin expects an optional numeric mode.");
  if (!running_ || !engine_) return info.Env().Undefined();
  auto begin = [&](Source s) {
    std::string detail;
    if (!engine_->Begin(s, &detail)) {
      const bool mic = s == Source::Microphone;
      pump_->Log(LogLevel::Error, std::string(mic ? "BeginMicrophone" : "BeginSystemAudio") +
                                      " failed: recorder restart failed. " + detail);
      pump_->Fatal("device_invalidated", std::string(mic ? "[mic:env_device_invalidated] " : "[sys:env_device_invalidated] ") +
                                             "Could not add the capture stream: " + detail);
    }
  };
  if (mode != PauseMode::SystemAudio) begin(Source::Microphone);
  if (mode != PauseMode::Microphone) begin(Source::SystemAudio);
  return info.Env().Undefined();
}

Napi::Value RecorderWrapper::Pause(const Napi::CallbackInfo& info) {
  const PauseMode mode = ParsePauseMode(info, "pause expects an optional numeric mode.");
  if (!running_ || !engine_) return info.Env().Undefined();
  if (mode != PauseMode::SystemAudio) engine_->Pause(Source::Microphone);
  if (mode != PauseMode::Microphone) engine_->Pause(Source::SystemAudio);
  return info.Env().Undefined();
}

Napi::Value RecorderWrapper::Stop(const Napi::CallbackInfo& info) {
  if (!running_) return info.Env().Undefined();
  StopInternal();
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value RecorderWrapper::Restart(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!running_ || !engine_) return Napi::Boolean::New(env, false);
  std::string detail;
  if (engine_->Restart(&detail)) return Napi::Boolean::New(env, true);
  StopInternal();
  return Napi::Boolean::New(env, false);
}

Napi::Value RecorderWrapper::SetAudioDevice(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Value arg = info.Length() > 0 ? info[0] : env.Undefined();
  DeviceSelection mic, spk;
  enum class Shape { Pair, Scalar } shape = Shape::Pair;
  if (arg.IsUndefined() || arg.IsNull()) {
    // Both back to the system default.
  } else if (arg.IsNumber()) {
    mic = SelectId(ParseUint32(env, arg, "deviceId", kSetDeviceRange));
    shape = Shape::Scalar;
  } else if (arg.IsString()) {
    mic = SelectUid(arg.As<Napi::String>().Utf8Value());
    shape = Shape::Scalar;
  } else if (arg.IsObject()) {
    Napi::Object o = arg.As<Napi::Object>();
    const bool hasMic = o.Has("microphone");
    const bool hasSpk = o.Has("speaker");
    if (hasMic || hasSpk) {
      if (hasMic) mic = ParseDeviceValue(env, o.Get("microphone"), "microphone", kSetDeviceRange);
      if (hasSpk) spk = ParseDeviceValue(env, o.Get("speaker"), "speaker", kSetDeviceRange);
    } else {
      mic = ParseDeviceValue(env, arg, "device", kSetDeviceRange);
    }
  } else {
    throw Napi::TypeError::New(env, "setAudioDevice expects number, string, object, or null.");
  }

  RegistrySnapshot snap;
  std::string error;
  const bool needSnapshot =
      mic.kind == DeviceSelection::Kind::Uid || spk.kind == DeviceSelection::Kind::Uid;
  const bool haveSnapshot = needSnapshot && TakeDeviceSnapshot(&snap, &error);

  if (running_ && engine_) {
    engine_->SetPreferredDevices(mic, spk);
  }

  if (shape == Shape::Scalar) return SelectionResult(env, mic, haveSnapshot ? &snap : nullptr, false);
  Napi::Object result = Napi::Object::New(env);
  result.Set("microphone", SelectionResult(env, mic, haveSnapshot ? &snap : nullptr, false));
  result.Set("speaker", SelectionResult(env, spk, haveSnapshot ? &snap : nullptr, true));
  return result;
}

Napi::Value RecorderWrapper::SetApmEnabled(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsBoolean()) {
    throw Napi::TypeError::New(env, "setApmEnabled requires a single boolean argument.");
  }
  if (running_ && engine_) engine_->SetApmEnabled(info[0].As<Napi::Boolean>().Value());
  return env.Undefined();
}

Napi::Value RecorderWrapper::FillSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    throw Napi::TypeError::New(env, "fillSilence requires a durationMs number argument.");
  }
  double ms = info[0].As<Napi::Number>().DoubleValue();
  if (!running_ || !engine_ || !(ms >= 10.0)) return env.Undefined();
  if (ms > kMaxFillSilenceMs) {
    pump_->Log(LogLevel::Warning, "fillSilence: " + std::to_string(static_cast<int64_t>(ms)) +
                                      " ms requested; capped at 24 h.");
    ms = kMaxFillSilenceMs;
  }
  engine_->FillSilence(static_cast<int>(ms));
  return env.Undefined();
}

Napi::Value RecorderWrapper::SetEncodingPaused(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsBoolean()) {
    throw Napi::TypeError::New(env, "setEncodingPaused requires a boolean argument.");
  }
  encodingPaused_ = info[0].As<Napi::Boolean>().Value();
  if (running_ && engine_) engine_->SetEncodingPaused(encodingPaused_);
  return env.Undefined();
}

void RecorderWrapper::StopInternal() {
  if (!running_) return;
  running_ = false;
  encodingPaused_ = false;
  if (cleanupHookAdded_) {
    napi_remove_env_cleanup_hook(env_, &RecorderWrapper::EnvCleanup, this);
    cleanupHookAdded_ = false;
  }
  std::unique_ptr<CaptureEngine> engine = std::move(engine_);
  std::shared_ptr<EventPump> pump = std::move(pump_);
  // Stops capture, drains the worker and finishes the Ogg stream; the final
  // pages and the closing logs are queued on the pump...
  engine->Stop();
  engine.reset();
  // ...and delivered to JS before stop() returns.
  pump->DropFatal();
  pump->Close(Napi::Env(env_));
}

void RecorderWrapper::AbandonRecording() {
  if (cleanupHookAdded_) {
    napi_remove_env_cleanup_hook(env_, &RecorderWrapper::EnvCleanup, this);
    cleanupHookAdded_ = false;
  }
  running_ = false;
  if (engine_) {
    engine_->Stop();
    engine_.reset();
  }
  if (pump_) {
    pump_->Abandon();
    pump_.reset();
  }
}

void RecorderWrapper::EnvCleanup(void* arg) {
  auto* self = static_cast<RecorderWrapper*>(arg);
  self->cleanupHookAdded_ = false;
  self->AbandonRecording();
}

}  // namespace plaud_rec
