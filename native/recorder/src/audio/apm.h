// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// webrtc-audio-processing 2.1 wrapper configured like the official recorder
// (InitializeOrUpdateApmLocked): 48 kHz mono, internal processing capped at
// 32 kHz, high-pass filter (not full band), AEC3 (non-mobile, enforced HPF)
// on demand, noise suppression kLow, AGC1 off, AGC2 adaptive digital
// (headroom 5 dB, max gain 30 dB, initial 8 dB, 3 dB/s, max noise -50 dBFS),
// no pre-amplifier, no capture level adjustment, no transient suppression.

#pragma once

#include <memory>
#include <string>

namespace webrtc {
class AudioProcessing;
}

namespace plaud_rec {

class ApmProcessor {
 public:
  ApmProcessor();
  ~ApmProcessor();
  ApmProcessor(const ApmProcessor&) = delete;
  ApmProcessor& operator=(const ApmProcessor&) = delete;

  bool Init(int sampleRate, bool echoCancellation, std::string* error);
  void SetEchoCancellation(bool enabled);
  bool echoCancellation() const { return aec_; }

  // 10 ms of mono render (loopback) audio.
  int ProcessReverse(const float* render, size_t frames);
  // 10 ms of mono capture audio; out may alias in.
  int ProcessCapture(const float* in, float* out, size_t frames, int streamDelayMs);

 private:
  void ApplyConfig();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  int sampleRate_ = 48000;
  bool aec_ = false;
};

}  // namespace plaud_rec
