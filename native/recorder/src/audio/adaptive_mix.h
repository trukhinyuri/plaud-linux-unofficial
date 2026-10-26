// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Adaptive two-source mixer used for "mixed" recordings (system audio +
// processed microphone), matching the official recorder's
// Util::ComputeAdaptiveMix + Util::mixChannelsWeighted:
//   per source: envelope follower on the chunk RMS (attack 0.7 when rising,
//   release 0.9 when falling), target gain 0.1/(env+1e-6) clamped to
//   [0.2, 2.5], microphone ducked by 0.5 while the system envelope exceeds
//   0.02, gains smoothed with 0.3*target + 0.7*previous, then scaled by the
//   user volumes; output = tanh(0.9 * (sys*gS + mic*gM) / (gS + gM)).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace plaud_rec {

class AdaptiveMixer {
 public:
  struct Gains {
    float system;
    float microphone;
  };

  void Reset() {
    sysEnv_ = 0.f;
    micEnv_ = 0.f;
    sysGain_ = 1.f;
    micGain_ = 1.f;
  }

  // Updates the smoothed gains from the RMS of the current chunks.
  Gains Update(float sysRms, float micRms, double systemVolume, double microphoneVolume) {
    sysEnv_ = Follow(sysEnv_, sysRms);
    micEnv_ = Follow(micEnv_, micRms);
    const float sysTarget = TargetGain(sysEnv_);
    float micTarget = TargetGain(micEnv_);
    if (sysEnv_ > kDuckThreshold) micTarget *= kDuckFactor;
    sysGain_ = kGainSmoothing * sysTarget + (1.f - kGainSmoothing) * sysGain_;
    micGain_ = kGainSmoothing * micTarget + (1.f - kGainSmoothing) * micGain_;
    return Gains{sysGain_ * static_cast<float>(systemVolume), micGain_ * static_cast<float>(microphoneVolume)};
  }

  static void Mix(const float* sys, const float* mic, size_t n, Gains g, float* out) {
    const float sum = g.system + g.microphone;
    if (!(sum > 1e-9f)) {
      std::fill(out, out + n, 0.f);
      return;
    }
    const float ws = kHeadroom * g.system / sum;
    const float wm = kHeadroom * g.microphone / sum;
    for (size_t i = 0; i < n; ++i) out[i] = std::tanh(sys[i] * ws + mic[i] * wm);
  }

 private:
  static constexpr float kTargetRms = 0.1f;
  static constexpr float kMinGain = 0.2f;
  static constexpr float kMaxGain = 2.5f;
  static constexpr float kDuckThreshold = 0.02f;
  static constexpr float kDuckFactor = 0.5f;
  static constexpr float kAttack = 0.7f;
  static constexpr float kRelease = 0.9f;
  static constexpr float kGainSmoothing = 0.3f;
  static constexpr float kHeadroom = 0.9f;

  static float Follow(float env, float rms) {
    const float c = env < rms ? kAttack : kRelease;
    return c * env + (1.f - c) * rms;
  }
  static float TargetGain(float env) { return std::clamp(kTargetRms / (env + 1e-6f), kMinGain, kMaxGain); }

  float sysEnv_ = 0.f;
  float micEnv_ = 0.f;
  float sysGain_ = 1.f;
  float micGain_ = 1.f;
};

}  // namespace plaud_rec
