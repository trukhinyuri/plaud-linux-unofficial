// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "meter_stream.h"

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace am {

namespace {

pw_stream_events gStreamEvents;

}  // namespace

MeterStream::MeterStream(PwEngine& engine, Kind kind, const DeviceNode& device, StartedFn onStarted)
    : engine_(engine), kind_(kind), uid_(device.name), name_(device.description), onStarted_(std::move(onStarted)) {}

MeterStream::~MeterStream() {
  if (stream_ != nullptr) {
    spa_hook_remove(&listener_);
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
}

std::string MeterStream::formatDescription() const {
  return std::to_string(rate_) + "Hz/" + std::to_string(channels_) + "ch/32bit/float/interleaved";
}

float MeterStream::takePeak() {
  const float p = peak_;
  peak_ = 0.0f;
  return p;
}

bool MeterStream::connect(std::string& error) {
  static std::once_flag once;
  std::call_once(once, [] {
    gStreamEvents.version = PW_VERSION_STREAM_EVENTS;
    gStreamEvents.state_changed = &MeterStream::onStateChanged;
    gStreamEvents.param_changed = &MeterStream::onParamChanged;
    gStreamEvents.process = &MeterStream::onProcess;
  });
  if (engine_.core() == nullptr) {
    error = "PipeWire is not connected";
    return false;
  }
  const bool mic = kind_ == Kind::Microphone;
  channels_ = mic ? 1 : 2;
  const char* nodeName = mic ? "plaud-mic-meter" : "plaud-speaker-meter";
  pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                           PW_KEY_NODE_NAME, nodeName, PW_KEY_NODE_DESCRIPTION,
                                           mic ? "Plaud smart microphone level" : "Plaud smart speaker level",
                                           PW_KEY_MEDIA_NAME, name_.c_str(), PW_KEY_TARGET_OBJECT, uid_.c_str(),
                                           PW_KEY_NODE_DONT_RECONNECT, "true", "node.dont-fallback", "true",
                                           "node.dont-move", "true", PW_KEY_STREAM_MONITOR, "true", nullptr);
  if (!mic) {
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");
  }
  stream_ = pw_stream_new(engine_.core(), nodeName, props);
  if (stream_ == nullptr) {
    error = std::strerror(errno);
    return false;
  }
  pw_stream_add_listener(stream_, &listener_, &gStreamEvents, this);

  uint8_t buffer[1024];
  spa_pod_builder builder;
  std::memset(&builder, 0, sizeof(builder));
  builder.data = buffer;
  builder.size = sizeof(buffer);
  spa_audio_info_raw info;
  std::memset(&info, 0, sizeof(info));
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = 48000;
  info.channels = channels_;
  if (mic) {
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  } else {
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
  }
  const spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
  const int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                                    static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                    params, 1);
  if (res < 0) {
    error = std::strerror(-res);
    spa_hook_remove(&listener_);
    pw_stream_destroy(stream_);
    stream_ = nullptr;
    return false;
  }
  return true;
}

void MeterStream::onStateChanged(void* data, pw_stream_state old, pw_stream_state state, const char* error) {
  auto* self = static_cast<MeterStream*>(data);
  self->streaming_ = state == PW_STREAM_STATE_STREAMING;
  if (state == PW_STREAM_STATE_ERROR) {
    self->failed_ = true;
    self->failure_ = error != nullptr ? error : "stream error";
  } else if (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED) {
    self->failed_ = true;
    self->failure_ = "stream disconnected";
  }
  if (self->streaming_ && !self->startedReported_) {
    self->startedReported_ = true;
    if (self->onStarted_) self->onStarted_(*self);
  }
}

void MeterStream::onParamChanged(void* data, uint32_t id, const spa_pod* param) {
  auto* self = static_cast<MeterStream*>(data);
  if (id != SPA_PARAM_Format || param == nullptr) return;
  spa_audio_info_raw info;
  std::memset(&info, 0, sizeof(info));
  if (spa_format_audio_raw_parse(param, &info) >= 0 && info.channels > 0) {
    self->rate_ = info.rate;
    self->channels_ = info.channels;
  }
}

void MeterStream::onProcess(void* data) {
  auto* self = static_cast<MeterStream*>(data);
  pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
  if (b == nullptr) return;
  spa_buffer* buf = b->buffer;
  if (buf->n_datas > 0 && buf->datas[0].data != nullptr && buf->datas[0].chunk != nullptr) {
    const spa_data& d = buf->datas[0];
    const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
    const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
    const float* samples = reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + offset);
    const size_t count = size / sizeof(float);
    const uint32_t channels = std::max<uint32_t>(1, self->channels_);
    if (self->kind_ == Kind::Microphone) {
      if (channels == 1) {
        self->meter_.process(samples, count);
      } else {
        // Channel average, as the official meters do for multi-channel input.
        static thread_local std::vector<float> mono;
        const size_t frames = count / channels;
        mono.resize(frames);
        for (size_t f = 0; f < frames; ++f) {
          float sum = 0.0f;
          for (uint32_t c = 0; c < channels; ++c) sum += samples[f * channels + c];
          mono[f] = sum / static_cast<float>(channels);
        }
        self->meter_.process(mono.data(), frames);
      }
    } else {
      float peak = self->peak_;
      for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::fabs(samples[i]));
      self->peak_ = peak;
    }
    if (count > 0) self->lastDataMs_ = steadyNowMs();
  }
  pw_stream_queue_buffer(self->stream_, b);
}

}  // namespace am
