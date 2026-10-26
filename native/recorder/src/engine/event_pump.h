// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Delivery of recorder events ({type: "log"|"audio"|"encoded"|"apm"|"fatal"})
// from native threads to the JS callback passed to Recorder#start.
//
// Producers append to a mutex-protected FIFO; a napi ThreadSafeFunction is
// used only as a coalesced wake-up of the JS thread, which then drains the
// FIFO in order. Close() drains synchronously on the JS thread, so the last
// encoded pages produced while stopping reach JS before stop() returns.

#pragma once

#include <napi.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "util/log.h"

namespace plaud_rec {

struct RecorderEvent {
  enum class Kind : uint8_t { Log, Audio, Encoded, Apm, Fatal };
  Kind kind = Kind::Log;
  std::string text1;  // log: level, fatal: code
  std::string text2;  // log/fatal: message
  std::vector<float> pcm;          // audio: processed 10 ms chunk
  std::vector<uint8_t> bytes;      // encoded: ogg page bytes
  double duration = 0;             // encoded: audio duration in ms
  double systemVolume = 0;         // audio: RMS of the system audio chunk
  double microphoneVolume = 0;     // audio: RMS of the microphone chunk
  std::vector<float> lpb, mic, enh;  // apm taps
  uint32_t sampleRate = 48000;
  uint32_t channels = 1;
};

class EventPump : public std::enable_shared_from_this<EventPump> {
 public:
  static std::shared_ptr<EventPump> Create(Napi::Env env, Napi::Function callback);
  ~EventPump();

  // Any thread.
  void Push(RecorderEvent&& ev);
  void Log(LogLevel level, std::string message);
  void Fatal(std::string code, std::string message);
  // Removes queued fatal events (a stop requested by JS supersedes them).
  void DropFatal();

  // JS thread only.
  void DrainSync(Napi::Env env);
  void Close(Napi::Env env);
  // Environment teardown: drop everything, deliver nothing, abort the TSFN.
  void Abandon();
  bool closed() const { return closed_.load(std::memory_order_acquire); }

 private:
  EventPump() = default;
  void Deliver(Napi::Env env, RecorderEvent& ev);

  std::mutex mu_;
  std::deque<RecorderEvent> queue_;
  size_t queuedAudio_ = 0;
  std::atomic<bool> wakePending_{false};
  std::atomic<bool> closed_{false};
  bool tsfnActive_ = false;
  Napi::ThreadSafeFunction tsfn_;
  Napi::FunctionReference callback_;
};

}  // namespace plaud_rec
