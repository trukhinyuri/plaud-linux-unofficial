// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// recorder_linux.node: exports { Recorder }, like the official addons.
// Loading the module does not touch PipeWire; connections are made by
// Recorder#start and the device statics when they are called.

#include <napi.h>

#include "recorder_wrapper.h"

namespace {

Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
  exports.Set("Recorder", plaud_rec::RecorderWrapper::Init(env));
  return exports;
}

}  // namespace

NODE_API_MODULE(recorder_linux, InitModule)
