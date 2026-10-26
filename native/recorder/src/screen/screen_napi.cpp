// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Argument validation mirrors the official recorder addon: same accepted
// shapes, error classes and messages, return types.

#include "screen_napi.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "screen_backend.h"

namespace plaud_rec {

namespace {

using screen::CaptureRequest;

// Invokes the optional JS log callback synchronously with {level, message}
// and `this` undefined.
class JsLogger {
 public:
  enum class OnThrow { kSwallow, kStopAndRethrow };

  JsLogger(Napi::Env env, Napi::Value callback, OnThrow mode) : env_(env), mode_(mode) {
    if (!callback.IsEmpty() && callback.IsFunction()) callback_ = callback.As<Napi::Function>();
  }

  screen::Log MakeLog(const std::string& prefix = std::string()) {
    if (callback_.IsEmpty()) return screen::Log();
    return screen::Log([this](const std::string& level, const std::string& message) { Emit(level, message); },
                       prefix);
  }

  // kStopAndRethrow: the first exception thrown by the callback ends logging
  // and is rethrown here once the native work is finished.
  void RethrowIfThrown() {
    if (!thrown_) return;
    Napi::Error error = std::move(*thrown_);
    thrown_.reset();
    throw error;
  }

 private:
  void Emit(const std::string& level, const std::string& message) {
    if (stopped_) return;
    Napi::HandleScope scope(env_);
    Napi::Object entry = Napi::Object::New(env_);
    entry.Set("level", Napi::String::New(env_, level));
    entry.Set("message", Napi::String::New(env_, message));
    try {
      callback_.Call(env_.Undefined(), {entry});
    } catch (Napi::Error& e) {
      if (mode_ == OnThrow::kStopAndRethrow) {
        stopped_ = true;
        thrown_.reset(new Napi::Error(std::move(e)));
      }
    }
  }

  Napi::Env env_;
  OnThrow mode_;
  Napi::Function callback_;
  bool stopped_ = false;
  std::unique_ptr<Napi::Error> thrown_;
};

[[noreturn]] void ThrowType(Napi::Env env, const char* message) { throw Napi::TypeError::New(env, message); }

[[noreturn]] void ThrowRange(Napi::Env env, const char* message) { throw Napi::RangeError::New(env, message); }

// Saturating double -> uint32 conversion (NaN and negatives become 0).
uint32_t ToUint32(double v) {
  if (!(v > 0)) return 0;
  if (v >= 4294967296.0) return 0xffffffffu;
  return static_cast<uint32_t>(v);
}

bool NumberProp(const Napi::Object& o, const char* key, double& out) {
  if (!o.Has(key)) return false;
  const Napi::Value v = o.Get(key);
  if (!v.IsNumber()) return false;
  out = v.As<Napi::Number>().DoubleValue();
  return true;
}

bool ArrayProp(const Napi::Object& o, const char* key, Napi::Array& out) {
  if (!o.Has(key)) return false;
  const Napi::Value v = o.Get(key);
  if (!v.IsArray()) return false;
  out = v.As<Napi::Array>();
  return true;
}

CaptureRequest ParseRect(Napi::Env env, const Napi::Value& value) {
  const Napi::Object r = value.As<Napi::Object>();
  CaptureRequest req;
  if (!NumberProp(r, "x", req.x) || !NumberProp(r, "y", req.y) || !NumberProp(r, "width", req.width) ||
      !NumberProp(r, "height", req.height)) {
    ThrowType(env, "rect must have numeric {x, y, width, height}.");
  }
  if (req.width <= 0 || req.height <= 0) ThrowRange(env, "rect width/height must be > 0.");
  req.kind = CaptureRequest::kRect;
  return req;
}

// One request object: displayId > displayIndex > windowId > rect > mode.
// Keys holding values of the wrong type are skipped.
CaptureRequest ParseRequest(Napi::Env env, const Napi::Object& o) {
  CaptureRequest req;
  double v = 0;
  if (NumberProp(o, "displayId", v)) {
    if (v < 0) ThrowRange(env, "displayId must be >= 0.");
    req.kind = CaptureRequest::kDisplayById;
    req.display_id = ToUint32(v);
    return req;
  }
  if (NumberProp(o, "displayIndex", v)) {
    if (v < 0) ThrowRange(env, "displayIndex must be >= 0.");
    req.kind = CaptureRequest::kDisplayByIndex;
    req.display_index = ToUint32(v);
    return req;
  }
  if (NumberProp(o, "windowId", v)) {
    if (v <= 0) ThrowRange(env, "windowId must be > 0.");
    req.kind = CaptureRequest::kWindowById;
    req.window_id = ToUint32(v);
    return req;
  }
  if (o.Has("rect")) {
    const Napi::Value rect = o.Get("rect");
    if (rect.IsObject()) return ParseRect(env, rect);
  }
  if (o.Has("mode")) {
    const Napi::Value mode = o.Get("mode");
    if (mode.IsString()) {
      const std::string m = mode.As<Napi::String>().Utf8Value();
      if (m == "main") {
        req.kind = CaptureRequest::kMainDisplay;
      } else if (m == "cursor") {
        req.kind = CaptureRequest::kCursorDisplay;
      } else if (m == "window") {
        req.kind = CaptureRequest::kCursorWindow;
      } else {
        ThrowType(env, "mode must be 'main', 'cursor', or 'window'.");
      }
    }
  }
  return req;
}

enum class IdKind { kDisplayId, kDisplayIndex, kWindowId };

void ParseIdArray(Napi::Env env, const Napi::Array& arr, IdKind kind, std::vector<CaptureRequest>& out) {
  static const char* const kEmpty[] = {"displayId array must be non-empty.", "displayIndex array must be non-empty.",
                                       "windowId array must be non-empty."};
  static const char* const kType[] = {"displayId array elements must be numbers.",
                                      "displayIndex array elements must be numbers.",
                                      "windowId array elements must be numbers."};
  const int k = static_cast<int>(kind);
  const uint32_t n = arr.Length();
  if (n == 0) ThrowType(env, kEmpty[k]);
  for (uint32_t i = 0; i < n; i++) {
    const Napi::Value e = arr.Get(i);
    if (!e.IsNumber()) ThrowType(env, kType[k]);
    const double v = e.As<Napi::Number>().DoubleValue();
    CaptureRequest req;
    switch (kind) {
      case IdKind::kDisplayId:
        if (v < 0) ThrowRange(env, "displayId must be >= 0.");
        req.kind = CaptureRequest::kDisplayById;
        req.display_id = ToUint32(v);
        break;
      case IdKind::kDisplayIndex:
        if (v < 0) ThrowRange(env, "displayIndex must be >= 0.");
        req.kind = CaptureRequest::kDisplayByIndex;
        req.display_index = ToUint32(v);
        break;
      case IdKind::kWindowId:
        if (v <= 0) ThrowRange(env, "windowId must be > 0.");
        req.kind = CaptureRequest::kWindowById;
        req.window_id = ToUint32(v);
        break;
    }
    out.push_back(req);
  }
}

void ParseObjectArray(Napi::Env env, const Napi::Array& arr, const char* empty_message, const char* type_message,
                      bool rects, std::vector<CaptureRequest>& out) {
  const uint32_t n = arr.Length();
  if (n == 0) ThrowType(env, empty_message);
  for (uint32_t i = 0; i < n; i++) {
    const Napi::Value e = arr.Get(i);
    if (!e.IsObject()) ThrowType(env, type_message);
    out.push_back(rects ? ParseRect(env, e) : ParseRequest(env, e.As<Napi::Object>()));
  }
}

void ParseOptions(Napi::Env env, const Napi::Value& value, std::vector<CaptureRequest>& out) {
  if (value.IsArray()) {
    ParseObjectArray(env, value.As<Napi::Array>(), "options array must be non-empty.",
                     "options array elements must be objects.", false, out);
    return;
  }
  const Napi::Object o = value.As<Napi::Object>();
  Napi::Array list;
  if (ArrayProp(o, "requests", list)) {
    ParseObjectArray(env, list, "requests array must be non-empty.", "requests array elements must be objects.", false,
                     out);
  } else if (ArrayProp(o, "displayId", list)) {
    ParseIdArray(env, list, IdKind::kDisplayId, out);
  } else if (ArrayProp(o, "displayIndex", list)) {
    ParseIdArray(env, list, IdKind::kDisplayIndex, out);
  } else if (ArrayProp(o, "windowId", list)) {
    ParseIdArray(env, list, IdKind::kWindowId, out);
  } else if (ArrayProp(o, "rect", list)) {
    ParseObjectArray(env, list, "rect array must be non-empty.", "rect array elements must be objects.", true, out);
  } else {
    out.push_back(ParseRequest(env, o));
  }
}

screen::HostInfo HostFromProcess(Napi::Env env) {
  screen::HostInfo host;
  try {
    const Napi::Value process = env.Global().Get("process");
    if (!process.IsObject()) return host;
    const Napi::Value versions = process.As<Napi::Object>().Get("versions");
    if (!versions.IsObject()) return host;
    const Napi::Value electron = versions.As<Napi::Object>().Get("electron");
    if (electron.IsString()) host.electron_major = std::atoi(electron.As<Napi::String>().Utf8Value().c_str());
  } catch (const Napi::Error&) {
    // A hostile getter on process.versions only loses the Electron hint.
  }
  return host;
}

Napi::Value OptionalCallbackArg(const Napi::CallbackInfo& info) {
  return info.Length() > 0 && info[0].IsFunction() ? info[0] : Napi::Value();
}

}  // namespace

Napi::Value CheckScreenCapturePermission(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  JsLogger logger(env, OptionalCallbackArg(info), JsLogger::OnThrow::kSwallow);
  const screen::PermissionStatus status = screen::CheckScreenCapturePermission(HostFromProcess(env), logger.MakeLog());
  return Napi::Number::New(env, static_cast<int>(status));
}

Napi::Value CheckScreenCapturePermissionFresh(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Value callback;
  const size_t argc = info.Length();
  if (argc == 1 && info[0].IsFunction()) {
    callback = info[0];
  } else if (argc == 2 && info[0].IsString() && info[1].IsFunction()) {
    callback = info[1];  // probePath: a macOS TCC probe helper; nothing to probe on Linux
  } else if (argc != 0) {
    ThrowType(env, "checkScreenCapturePermissionFresh expects (callback) or (probePath, callback)");
  }
  JsLogger logger(env, callback, JsLogger::OnThrow::kSwallow);
  const screen::PermissionStatus status = screen::CheckScreenCapturePermission(HostFromProcess(env), logger.MakeLog());
  return Napi::Number::New(env, static_cast<int>(status));
}

Napi::Value RequestScreenCapturePermission(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  JsLogger logger(env, OptionalCallbackArg(info), JsLogger::OnThrow::kSwallow);
  const screen::PermissionStatus status = screen::CheckScreenCapturePermission(HostFromProcess(env), logger.MakeLog());
  return Napi::Boolean::New(env, status == screen::PermissionStatus::Granted);
}

Napi::Value CaptureScreenImage(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t argc = info.Length();
  std::vector<CaptureRequest> requests;
  Napi::Value callback;
  if (argc == 0 || info[0].IsUndefined() || info[0].IsNull()) {
    requests.emplace_back();
  } else if (info[0].IsFunction()) {
    callback = info[0];
    requests.emplace_back();
  } else if (info[0].IsObject()) {
    if (argc >= 2) {
      if (info[1].IsFunction()) {
        callback = info[1];
      } else if (!info[1].IsUndefined() && !info[1].IsNull()) {
        ThrowType(env, "Second argument must be a callback function.");
      }
    }
    ParseOptions(env, info[0], requests);
  } else {
    ThrowType(env, "First argument must be an options object or a callback function.");
  }
  if (requests.empty()) throw Napi::Error::New(env, "No capture requests provided.");

  JsLogger logger(env, callback, JsLogger::OnThrow::kStopAndRethrow);
  screen::ScreenCapturer capturer(HostFromProcess(env));
  if (requests.size() == 1) {
    const std::vector<uint8_t> png = capturer.Capture(requests[0], logger.MakeLog());
    logger.RethrowIfThrown();
    if (png.empty()) throw Napi::Error::New(env, "Failed to capture screen image.");
    return Napi::Buffer<uint8_t>::Copy(env, png.data(), png.size());
  }
  Napi::Array result = Napi::Array::New(env, requests.size());
  for (size_t i = 0; i < requests.size(); i++) {
    const std::vector<uint8_t> png = capturer.Capture(requests[i], logger.MakeLog("[" + std::to_string(i) + "] "));
    logger.RethrowIfThrown();
    if (png.empty()) {
      throw Napi::Error::New(env, "Failed to capture screen image (batch index " + std::to_string(i) + ").");
    }
    result.Set(static_cast<uint32_t>(i), Napi::Buffer<uint8_t>::Copy(env, png.data(), png.size()));
  }
  return result;
}

Napi::Value GetWindowAtCursor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t argc = info.Length();
  if (argc > 1 || (argc == 1 && !info[0].IsFunction())) ThrowType(env, "getWindowAtCursor expects an optional callback");
  JsLogger logger(env, argc == 1 ? info[0] : Napi::Value(), JsLogger::OnThrow::kSwallow);
  screen::ScreenCapturer capturer(HostFromProcess(env));
  screen::WindowInfo window;
  if (!capturer.WindowAtCursor(window, logger.MakeLog())) return env.Null();
  Napi::Object out = Napi::Object::New(env);
  out.Set("windowId", Napi::Number::New(env, window.window_id));
  out.Set("x", Napi::Number::New(env, window.x));
  out.Set("y", Napi::Number::New(env, window.y));
  out.Set("width", Napi::Number::New(env, window.width));
  out.Set("height", Napi::Number::New(env, window.height));
  out.Set("ownerName", Napi::String::New(env, window.owner_name));
  out.Set("windowName", Napi::String::New(env, window.window_name));
  return out;
}

Napi::Value ExtractTextFromImage(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const size_t argc = info.Length();
  if (argc == 0 || !info[0].IsBuffer()) ThrowType(env, "First argument must be a PNG Buffer.");
  Napi::Value callback;
  if (argc >= 2) {
    if (info[1].IsFunction()) {
      callback = info[1];
    } else if (!info[1].IsUndefined() && !info[1].IsNull()) {
      ThrowType(env, "Second argument must be a callback function.");
    }
  }
  // Same contract as the official Windows build: no OCR engine is bundled,
  // the call reports it and fails.
  JsLogger logger(env, callback, JsLogger::OnThrow::kSwallow);
  logger.MakeLog().Warn("OCR is not implemented on Linux in this build.");
  throw Napi::Error::New(env, "Failed to extract text from image (Vision OCR).");
}

}  // namespace plaud_rec
