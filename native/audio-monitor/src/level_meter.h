// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Voice-band level meter used by the smart microphone monitor, identical to
// the official addons' per-device meter (same coefficients in the macOS and
// Windows binaries):
//   mono input (channel average) -> high-pass biquad (~120 Hz) -> low-pass
//   biquad (~3.2 kHz), both transposed direct form II, coefficients for
//   48 kHz -> RMS per buffer -> asymmetric smoothing
//   (attack 0.4 when the RMS rises, release 0.05 when it falls).
// The PipeWire capture streams request F32 mono at 48 kHz, so the filter
// sees exactly the sample rate its coefficients were designed for.
#pragma once

#include <cmath>
#include <cstddef>

namespace am {

class VoiceLevelMeter {
 public:
  static constexpr double kHpB0 = 0.9816582826;
  static constexpr double kHpB1 = -1.9633165651;
  static constexpr double kHpB2 = 0.9816582826;
  static constexpr double kHpA1 = -1.9631912746;
  static constexpr double kHpA2 = 0.9634418556;
  static constexpr double kLpB0 = 0.0335593801;
  static constexpr double kLpB1 = 0.0671187602;
  static constexpr double kLpB2 = 0.0335593801;
  static constexpr double kLpA1 = -1.4180655436;
  static constexpr double kLpA2 = 0.5523030641;
  static constexpr float kAttack = 0.4f;
  static constexpr float kRelease = 0.05f;

  // Processes one buffer of mono samples and updates the smoothed level.
  void process(const float* samples, size_t frames) {
    if (frames == 0) return;
    double sumSquares = 0.0;
    for (size_t i = 0; i < frames; ++i) {
      const double x = samples[i];
      const double hp = kHpB0 * x + hpZ1_;
      hpZ1_ = kHpB1 * x - kHpA1 * hp + hpZ2_;
      hpZ2_ = kHpB2 * x - kHpA2 * hp;
      const double lp = kLpB0 * hp + lpZ1_;
      lpZ1_ = kLpB1 * hp - kLpA1 * lp + lpZ2_;
      lpZ2_ = kLpB2 * hp - kLpA2 * lp;
      sumSquares += lp * lp;
    }
    const float rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(frames)));
    const float alpha = rms > level_ ? kAttack : kRelease;
    level_ += (rms - level_) * alpha;
    if (!std::isfinite(level_)) reset();
  }

  float level() const { return level_; }
  void reset() {
    hpZ1_ = hpZ2_ = lpZ1_ = lpZ2_ = 0.0;
    level_ = 0.0f;
  }

 private:
  double hpZ1_ = 0.0, hpZ2_ = 0.0, lpZ1_ = 0.0, lpZ2_ = 0.0;
  float level_ = 0.0f;
};

}  // namespace am
