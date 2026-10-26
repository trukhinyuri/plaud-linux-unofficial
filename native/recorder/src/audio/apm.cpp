// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "audio/apm.h"

#include <modules/audio_processing/include/audio_processing.h>

#include <vector>

namespace plaud_rec {

struct ApmProcessor::Impl {
  rtc::scoped_refptr<webrtc::AudioProcessing> apm;
  std::vector<float> renderScratch;
};

ApmProcessor::ApmProcessor() : impl_(new Impl()) {}
ApmProcessor::~ApmProcessor() = default;

bool ApmProcessor::Init(int sampleRate, bool echoCancellation, std::string* error) {
  sampleRate_ = sampleRate;
  aec_ = echoCancellation;
  impl_->apm = webrtc::AudioProcessingBuilder().Create();
  if (!impl_->apm) {
    if (error) *error = "Failed to create APM.";
    return false;
  }
  ApplyConfig();
  const webrtc::StreamConfig mono(sampleRate_, 1);
  webrtc::ProcessingConfig processing = {{mono, mono, mono, mono}};
  const int rc = impl_->apm->Initialize(processing);
  if (rc != webrtc::AudioProcessing::kNoError) {
    if (error) *error = "APM Initialize returned error: " + std::to_string(rc);
    impl_->apm = nullptr;
    return false;
  }
  impl_->renderScratch.assign(static_cast<size_t>(sampleRate_ / 100), 0.f);
  return true;
}

void ApmProcessor::ApplyConfig() {
  webrtc::AudioProcessing::Config cfg;
  cfg.pipeline.maximum_internal_processing_rate = 32000;
  cfg.pipeline.multi_channel_render = false;
  cfg.pipeline.multi_channel_capture = false;
  cfg.pre_amplifier.enabled = false;
  cfg.capture_level_adjustment.enabled = false;
  cfg.high_pass_filter.enabled = true;
  cfg.high_pass_filter.apply_in_full_band = false;
  cfg.echo_canceller.enabled = aec_;
  cfg.echo_canceller.mobile_mode = false;
  cfg.echo_canceller.enforce_high_pass_filtering = true;
  cfg.noise_suppression.enabled = true;
  cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
  cfg.transient_suppression.enabled = false;
  cfg.gain_controller1.enabled = false;
  cfg.gain_controller2.enabled = true;
  cfg.gain_controller2.input_volume_controller.enabled = false;
  cfg.gain_controller2.adaptive_digital.enabled = true;
  cfg.gain_controller2.adaptive_digital.headroom_db = 5.f;
  cfg.gain_controller2.adaptive_digital.max_gain_db = 30.f;
  cfg.gain_controller2.adaptive_digital.initial_gain_db = 8.f;
  cfg.gain_controller2.adaptive_digital.max_gain_change_db_per_second = 3.f;
  cfg.gain_controller2.adaptive_digital.max_output_noise_level_dbfs = -50.f;
  cfg.gain_controller2.fixed_digital.gain_db = 0.f;
  impl_->apm->ApplyConfig(cfg);
}

void ApmProcessor::SetEchoCancellation(bool enabled) {
  if (!impl_->apm || enabled == aec_) return;
  aec_ = enabled;
  ApplyConfig();
}

int ApmProcessor::ProcessReverse(const float* render, size_t frames) {
  if (!impl_->apm) return webrtc::AudioProcessing::kNullPointerError;
  const webrtc::StreamConfig mono(sampleRate_, 1);
  if (impl_->renderScratch.size() < frames) impl_->renderScratch.resize(frames);
  const float* src[1] = {render};
  float* dst[1] = {impl_->renderScratch.data()};
  return impl_->apm->ProcessReverseStream(src, mono, mono, dst);
}

int ApmProcessor::ProcessCapture(const float* in, float* out, size_t frames, int streamDelayMs) {
  (void)frames;
  if (!impl_->apm) return webrtc::AudioProcessing::kNullPointerError;
  const webrtc::StreamConfig mono(sampleRate_, 1);
  impl_->apm->set_stream_delay_ms(streamDelayMs);
  const float* src[1] = {in};
  float* dst[1] = {out};
  return impl_->apm->ProcessStream(src, mono, mono, dst);
}

}  // namespace plaud_rec
