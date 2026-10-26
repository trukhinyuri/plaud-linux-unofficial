// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "aec_metrics_napi.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "aec_metrics.h"

namespace plaud_rec {
namespace {

// A float view of a JS buffer argument. Node hands out Buffer slices at any
// byte offset; unaligned data is copied so the math only sees aligned floats.
struct FloatInput {
  const float* data = nullptr;
  size_t length = 0;
  std::vector<float> aligned;

  void Set(const void* raw, size_t count) {
    length = count;
    if (raw == nullptr || count == 0 || reinterpret_cast<uintptr_t>(raw) % alignof(float) == 0) {
      data = static_cast<const float*>(raw);
      return;
    }
    aligned.resize(count);
    std::memcpy(aligned.data(), raw, count * sizeof(float));
    data = aligned.data();
  }
};

// Bytes per element by napi_typedarray_type (int8 .. biguint64); unknown kinds count as 0.
size_t ElementSize(napi_typedarray_type type) {
  static const size_t kSizes[] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8};
  const auto index = static_cast<unsigned>(type);
  return index < sizeof(kSizes) / sizeof(kSizes[0]) ? kSizes[index] : 0;
}

// Any ArrayBufferView that napi reports as a buffer (Buffer, every TypedArray)
// is reinterpreted as floats: byteLength / 4 samples. A DataView passes the
// buffer check but has no typed-array info, which surfaces as napi's
// "Invalid argument" Error.
bool GetFloatBuffer(const Napi::Env& env, const Napi::Value& value, const char* name, FloatInput& out) {
  if (value.IsBuffer()) {
    napi_typedarray_type type;
    size_t length = 0;
    void* data = nullptr;
    napi_value arrayBuffer;
    size_t byteOffset = 0;
    const napi_status status =
        napi_get_typedarray_info(env, value, &type, &length, &data, &arrayBuffer, &byteOffset);
    NAPI_THROW_IF_FAILED(env, status, false);
    out.Set(data, (length * ElementSize(type)) >> 2);
    return true;
  }
  if (value.IsTypedArray() && value.As<Napi::TypedArray>().TypedArrayType() == napi_float32_array) {
    const auto array = value.As<Napi::Float32Array>();
    out.Set(array.Data(), array.ElementLength());
    return true;
  }
  const std::string message = std::string(name) + " must be a Buffer<float> or Float32Array.";
  Napi::TypeError::New(env, message).ThrowAsJavaScriptException();
  return false;
}

void ReadDouble(const Napi::Object& options, const char* key, double& target) {
  if (!options.Has(key)) return;
  const Napi::Value value = options.Get(key);
  if (value.IsNumber()) target = value.As<Napi::Number>().DoubleValue();
}

// Integer options are fetched twice (type check, then value), like the reference.
void ReadInt(const Napi::Object& options, const char* key, int& target) {
  if (!options.Has(key)) return;
  if (!options.Get(key).IsNumber()) return;
  target = options.Get(key).As<Napi::Number>().Int32Value();
}

void ReadOptions(const Napi::Object& options, aec_metrics::Options& opt) {
  ReadDouble(options, "frameMs", opt.frameMs);
  ReadDouble(options, "hopMs", opt.hopMs);
  ReadDouble(options, "maxDelayMs", opt.maxDelayMs);
  ReadDouble(options, "corrThresh", opt.corrThresh);
  ReadDouble(options, "lpbRelThreshDb", opt.lpbRelThreshDb);
  ReadDouble(options, "delayEstimationMaxSec", opt.delayEstimationMaxSec);
  ReadDouble(options, "residualEchoCorrThresh", opt.residualEchoCorrThresh);
  ReadDouble(options, "noiseMaxDbAboveFloor", opt.noiseMaxDbAboveFloor);
  ReadDouble(options, "speechMinDbAboveNoise", opt.speechMinDbAboveNoise);
  ReadDouble(options, "ansSuppressionDbThreshold1", opt.ansSuppressionDbThreshold1);
  ReadDouble(options, "ansSuppressionDbThreshold2", opt.ansSuppressionDbThreshold2);
  ReadDouble(options, "agcSpeechTargetDbfs", opt.agcSpeechTargetDbfs);
  ReadDouble(options, "agcSpeechToleranceDb", opt.agcSpeechToleranceDb);
  ReadInt(options, "downsampleFactorForDelay", opt.downsampleFactorForDelay);
  ReadInt(options, "targetSampleRate", opt.targetSampleRate);
  ReadInt(options, "minNoiseFrames", opt.minNoiseFrames);
  ReadInt(options, "minSpeechFrames", opt.minSpeechFrames);
}

Napi::Object ToObject(const Napi::Env& env, const aec_metrics::Result& result) {
  Napi::Object out = Napi::Object::New(env);
  for (const auto& field : aec_metrics::kIntFields) {
    out.Set(field.name, Napi::Number::New(env, static_cast<double>(result.*field.member)));
  }
  for (const auto& field : aec_metrics::kOptionalFields) {
    const std::optional<double>& value = result.*field.member;
    if (value.has_value()) {
      out.Set(field.name, Napi::Number::New(env, *value));
    } else {
      out.Set(field.name, env.Null());
    }
  }
  return out;
}

}  // namespace

Napi::Value ComputeAecLightMetrics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() <= 5) {
    Napi::TypeError::New(env, "computeAecLightMetrics requires (lpbBuf, lpbSr, micBuf, micSr, enhBuf, enhSr, [options]).")
        .ThrowAsJavaScriptException();
    return env.Null();
  }
  FloatInput lpb;
  FloatInput mic;
  FloatInput enh;
  if (!GetFloatBuffer(env, info[0], "lpbBuf", lpb)) return env.Null();
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "lpbSr must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (!GetFloatBuffer(env, info[2], "micBuf", mic)) return env.Null();
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "micSr must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (!GetFloatBuffer(env, info[4], "enhBuf", enh)) return env.Null();
  if (!info[5].IsNumber()) {
    Napi::TypeError::New(env, "enhSr must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }
  const int lpbSr = info[1].As<Napi::Number>().Int32Value();
  const int micSr = info[3].As<Napi::Number>().Int32Value();
  const int enhSr = info[5].As<Napi::Number>().Int32Value();

  aec_metrics::Options options;
  if (info.Length() >= 7 && info[6].IsObject()) ReadOptions(info[6].As<Napi::Object>(), options);

  aec_metrics::Result result;
  try {
    result = aec_metrics::ComputeAecLightMetrics(lpb.data, lpb.length, lpbSr, mic.data, mic.length, micSr, enh.data,
                                                 enh.length, enhSr, options);
  } catch (const std::exception& e) {
    Napi::Error::New(env, std::string("computeAecLightMetrics failed: ") + e.what()).ThrowAsJavaScriptException();
    return env.Null();
  }
  return ToObject(env, result);
}

}  // namespace plaud_rec
