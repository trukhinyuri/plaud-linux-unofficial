// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// N-API entry points of the screen-capture statics. Registered on the
// exported Recorder class as checkScreenCapturePermission,
// checkScreenCapturePermissionFresh, requestScreenCapturePermission,
// captureScreenImage, getWindowAtCursor and extractTextFromImage.

#pragma once

#include <napi.h>

namespace plaud_rec {

Napi::Value CheckScreenCapturePermission(const Napi::CallbackInfo& info);
Napi::Value CheckScreenCapturePermissionFresh(const Napi::CallbackInfo& info);
Napi::Value RequestScreenCapturePermission(const Napi::CallbackInfo& info);
Napi::Value CaptureScreenImage(const Napi::CallbackInfo& info);
Napi::Value GetWindowAtCursor(const Napi::CallbackInfo& info);
Napi::Value ExtractTextFromImage(const Napi::CallbackInfo& info);

}  // namespace plaud_rec
