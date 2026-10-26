// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "engine/event_pump.h"

#include <utility>

namespace plaud_rec {

namespace {

// Volume ("audio") events are informational; if the JS thread stalls for a
// long time they are the only ones dropped, so memory stays bounded while
// encoded audio, logs and fatal events are never lost.
constexpr size_t kMaxQueuedAudioEvents = 3000;  // 30 s of 10 ms chunks

}  // namespace

std::shared_ptr<EventPump> EventPump::Create(Napi::Env env, Napi::Function callback) {
  std::shared_ptr<EventPump> pump(new EventPump());
  pump->callback_ = Napi::Persistent(callback);
  pump->tsfn_ = Napi::ThreadSafeFunction::New(env, callback, "RecorderCallback", 0, 1);
  pump->tsfnActive_ = true;
  return pump;
}

EventPump::~EventPump() = default;

void EventPump::Push(RecorderEvent&& ev) {
  std::lock_guard<std::mutex> lock(mu_);
  if (closed_.load(std::memory_order_relaxed)) return;
  if (ev.kind == RecorderEvent::Kind::Audio) {
    if (queuedAudio_ >= kMaxQueuedAudioEvents) return;
    ++queuedAudio_;
  }
  queue_.push_back(std::move(ev));
  if (!wakePending_.exchange(true, std::memory_order_acq_rel) && tsfnActive_) {
    std::weak_ptr<EventPump> weak = shared_from_this();
    const napi_status st = tsfn_.NonBlockingCall([weak](Napi::Env env, Napi::Function) {
      if (auto self = weak.lock()) self->DrainSync(env);
    });
    if (st != napi_ok) wakePending_.store(false, std::memory_order_release);
  }
}

void EventPump::Log(LogLevel level, std::string message) {
  RecorderEvent ev;
  ev.kind = RecorderEvent::Kind::Log;
  ev.text1 = LogLevelName(level);
  ev.text2 = std::move(message);
  Push(std::move(ev));
}

void EventPump::Fatal(std::string code, std::string message) {
  RecorderEvent ev;
  ev.kind = RecorderEvent::Kind::Fatal;
  ev.text1 = std::move(code);
  ev.text2 = std::move(message);
  Push(std::move(ev));
}

void EventPump::DropFatal() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = queue_.begin(); it != queue_.end();) {
    it = it->kind == RecorderEvent::Kind::Fatal ? queue_.erase(it) : std::next(it);
  }
}

void EventPump::DrainSync(Napi::Env env) {
  wakePending_.store(false, std::memory_order_release);
  for (;;) {
    RecorderEvent ev;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (queue_.empty()) break;
      ev = std::move(queue_.front());
      queue_.pop_front();
      if (ev.kind == RecorderEvent::Kind::Audio) --queuedAudio_;
    }
    Deliver(env, ev);
  }
}

void EventPump::Close(Napi::Env env) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    closed_.store(true, std::memory_order_release);
  }
  DrainSync(env);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (tsfnActive_) {
      tsfnActive_ = false;
      tsfn_.Release();
    }
  }
  callback_.Reset();
}

void EventPump::Abandon() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    closed_.store(true, std::memory_order_release);
    queue_.clear();
    queuedAudio_ = 0;
    if (tsfnActive_) {
      tsfnActive_ = false;
      tsfn_.Abort();
    }
  }
  callback_.Reset();
}

void EventPump::Deliver(Napi::Env env, RecorderEvent& ev) {
  if (callback_.IsEmpty()) return;
  Napi::HandleScope scope(env);
  Napi::Object obj = Napi::Object::New(env);
  switch (ev.kind) {
    case RecorderEvent::Kind::Log:
      obj.Set("type", "log");
      obj.Set("level", ev.text1);
      obj.Set("message", ev.text2);
      break;
    case RecorderEvent::Kind::Audio:
      obj.Set("type", "audio");
      obj.Set("data", Napi::Buffer<float>::Copy(env, ev.pcm.data(), ev.pcm.size()));
      obj.Set("container", "pcm");
      obj.Set("size", Napi::Number::New(env, static_cast<double>(ev.pcm.size())));
      obj.Set("systemVolume", Napi::Number::New(env, ev.systemVolume));
      obj.Set("microphoneVolume", Napi::Number::New(env, ev.microphoneVolume));
      break;
    case RecorderEvent::Kind::Encoded:
      obj.Set("type", "encoded");
      obj.Set("data", Napi::Buffer<uint8_t>::Copy(env, ev.bytes.data(), ev.bytes.size()));
      obj.Set("container", "ogg");
      obj.Set("size", Napi::Number::New(env, static_cast<double>(ev.bytes.size())));
      obj.Set("duration", Napi::Number::New(env, ev.duration));
      break;
    case RecorderEvent::Kind::Apm:
      obj.Set("type", "apm");
      obj.Set("lpb", Napi::Buffer<float>::Copy(env, ev.lpb.data(), ev.lpb.size()));
      obj.Set("mic", Napi::Buffer<float>::Copy(env, ev.mic.data(), ev.mic.size()));
      obj.Set("enh", Napi::Buffer<float>::Copy(env, ev.enh.data(), ev.enh.size()));
      obj.Set("lpbSampleRate", Napi::Number::New(env, ev.sampleRate));
      obj.Set("micSampleRate", Napi::Number::New(env, ev.sampleRate));
      obj.Set("enhSampleRate", Napi::Number::New(env, ev.sampleRate));
      obj.Set("lpbChannels", Napi::Number::New(env, ev.channels));
      obj.Set("micChannels", Napi::Number::New(env, ev.channels));
      obj.Set("enhChannels", Napi::Number::New(env, ev.channels));
      break;
    case RecorderEvent::Kind::Fatal:
      obj.Set("type", "fatal");
      obj.Set("code", ev.text1);
      obj.Set("message", ev.text2);
      break;
  }
  try {
    callback_.Call({obj});
  } catch (const Napi::Error&) {
    // An exception thrown by the JS handler must not unwind into the native
    // drain loop; the official addons also swallow it ("Error in JS ...
    // callback"). The exception is cleared by catching it here.
  }
}

}  // namespace plaud_rec
