// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// One metering capture stream on a device node (no audio is stored):
//   * Microphone: F32 mono 48 kHz from an Audio/Source node, fed to the
//     VoiceLevelMeter (smart microphone);
//   * SinkMonitor: passive F32 stereo capture of an Audio/Sink's monitor
//     (stream.capture.sink, node.passive: never wakes an idle output),
//     reduced to the absolute peak between two reads (smart speaker).
// The stream targets the device by node.name with node.dont-reconnect and
// node.dont-fallback, so it never silently follows another device. Process
// callbacks run on the engine's loop thread (no RT flag), the same thread as
// the monitors' timers, so the meters need no locking.
#pragma once

#include <pipewire/pipewire.h>

#include <functional>
#include <string>

#include "graph_types.h"
#include "level_meter.h"
#include "pw_engine.h"

namespace am {

class MeterStream {
 public:
  enum class Kind { Microphone, SinkMonitor };
  // Called once when the stream starts streaming (format negotiated).
  using StartedFn = std::function<void(MeterStream&)>;

  MeterStream(PwEngine& engine, Kind kind, const DeviceNode& device, StartedFn onStarted);
  ~MeterStream();
  MeterStream(const MeterStream&) = delete;
  MeterStream& operator=(const MeterStream&) = delete;

  // Creates and connects the stream (lock held). False on failure (`error`).
  bool connect(std::string& error);

  const std::string& uid() const { return uid_; }
  const std::string& name() const { return name_; }
  bool failed() const { return failed_; }
  const std::string& failure() const { return failure_; }
  bool streaming() const { return streaming_; }
  std::string formatDescription() const;

  float level() const { return meter_.level(); }      // Microphone
  float takePeak();                                     // SinkMonitor
  int64_t lastDataMs() const { return lastDataMs_; }

 private:
  static void onStateChanged(void* data, pw_stream_state old, pw_stream_state state, const char* error);
  static void onParamChanged(void* data, uint32_t id, const spa_pod* param);
  static void onProcess(void* data);

  PwEngine& engine_;
  Kind kind_;
  std::string uid_;
  std::string name_;
  StartedFn onStarted_;
  pw_stream* stream_ = nullptr;
  spa_hook listener_{};
  uint32_t channels_ = 1;
  uint32_t rate_ = 48000;
  bool streaming_ = false;
  bool startedReported_ = false;
  bool failed_ = false;
  std::string failure_;
  VoiceLevelMeter meter_;
  float peak_ = 0.0f;
  int64_t lastDataMs_ = 0;
};

}  // namespace am
