// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

// Recorder.computeAecLightMetrics(lpbBuf, lpbSr, micBuf, micSr, enhBuf, enhSr, [options])
// Registered by the Recorder class as a static method.

#ifndef PLAUD_REC_AEC_METRICS_NAPI_H
#define PLAUD_REC_AEC_METRICS_NAPI_H

#include <napi.h>

namespace plaud_rec {

Napi::Value ComputeAecLightMetrics(const Napi::CallbackInfo& info);

}  // namespace plaud_rec

#endif  // PLAUD_REC_AEC_METRICS_NAPI_H
