// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// The JS-visible `Recorder` class: same methods, statics, argument
// validation, error messages and return shapes as Plaud's official
// recorder addons, backed by the PipeWire capture engine.

#pragma once

#include <napi.h>

#include <memory>

#include "engine/capture_engine.h"
#include "engine/event_pump.h"

namespace plaud_rec {

class RecorderWrapper : public Napi::ObjectWrap<RecorderWrapper> {
 public:
  static Napi::Function Init(Napi::Env env);
  explicit RecorderWrapper(const Napi::CallbackInfo& info);
  ~RecorderWrapper() override;

 private:
  Napi::Value Start(const Napi::CallbackInfo& info);
  Napi::Value Begin(const Napi::CallbackInfo& info);
  Napi::Value Pause(const Napi::CallbackInfo& info);
  Napi::Value Stop(const Napi::CallbackInfo& info);
  Napi::Value Restart(const Napi::CallbackInfo& info);
  Napi::Value SetAudioDevice(const Napi::CallbackInfo& info);
  Napi::Value SetApmEnabled(const Napi::CallbackInfo& info);
  Napi::Value FillSilence(const Napi::CallbackInfo& info);
  Napi::Value SetEncodingPaused(const Napi::CallbackInfo& info);

  void StopInternal();
  void AbandonRecording();
  static void EnvCleanup(void* arg);

  napi_env env_ = nullptr;
  std::shared_ptr<EventPump> pump_;
  std::unique_ptr<CaptureEngine> engine_;
  bool running_ = false;
  bool encodingPaused_ = false;
  bool cleanupHookAdded_ = false;
};

}  // namespace plaud_rec
