// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "pw/pw_connection.h"

#include <spa/utils/result.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace plaud_rec {

void EnsurePipeWireInit() {
  static std::once_flag once;
  std::call_once(once, [] {
    pw_init(nullptr, nullptr);
    if (std::getenv("PIPEWIRE_DEBUG") == nullptr) pw_log_set_level(SPA_LOG_LEVEL_NONE);
  });
}

namespace {

pw_core_events MakeCoreEvents(void (*info)(void*, const pw_core_info*), void (*done)(void*, uint32_t, int),
                              void (*error)(void*, uint32_t, int, int, const char*)) {
  pw_core_events ev{};
  ev.version = PW_VERSION_CORE_EVENTS;
  ev.info = info;
  ev.done = done;
  ev.error = error;
  return ev;
}

}  // namespace

PwConnection::PwConnection(std::string threadName) : threadName_(std::move(threadName)) {}

PwConnection::~PwConnection() { Disconnect(); }

bool PwConnection::Connect(int timeoutMs, std::string* error) {
  static const pw_core_events kEvents = MakeCoreEvents(&PwConnection::OnCoreInfo, &PwConnection::OnCoreDone,
                                                       &PwConnection::OnCoreError);
  EnsurePipeWireInit();
  Disconnect();
  broken_ = false;
  loop_ = pw_thread_loop_new(threadName_.c_str(), nullptr);
  if (loop_ == nullptr) {
    if (error) *error = std::string("pw_thread_loop_new failed: ") + std::strerror(errno);
    return false;
  }
  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (context_ == nullptr) {
    if (error) *error = std::string("pw_context_new failed: ") + std::strerror(errno);
    Disconnect();
    return false;
  }
  if (pw_thread_loop_start(loop_) < 0) {
    if (error) *error = "pw_thread_loop_start failed";
    Disconnect();
    return false;
  }
  bool ok = false;
  {
    PwLock lock(*this);
    pw_properties* props = pw_properties_new(PW_KEY_APP_NAME, "Plaud", nullptr);
    core_ = pw_context_connect(context_, props, 0);
    if (core_ == nullptr) {
      if (error) *error = std::string("cannot connect to the PipeWire daemon: ") + std::strerror(errno);
    } else {
      pw_core_add_listener(core_, &coreListener_, &kEvents, this);
      listenerAdded_ = true;
      ok = SyncLocked(timeoutMs);
      if (!ok && error) *error = broken_ ? "PipeWire daemon closed the connection" : "PipeWire daemon did not respond";
    }
  }
  if (!ok) Disconnect();
  return ok;
}

void PwConnection::Disconnect() {
  if (loop_ != nullptr) pw_thread_loop_stop(loop_);
  if (listenerAdded_) {
    spa_hook_remove(&coreListener_);
    listenerAdded_ = false;
  }
  if (core_ != nullptr) {
    pw_core_disconnect(core_);
    core_ = nullptr;
  }
  if (context_ != nullptr) {
    pw_context_destroy(context_);
    context_ = nullptr;
  }
  if (loop_ != nullptr) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
}

bool PwConnection::SyncLocked(int timeoutMs) {
  if (core_ == nullptr || broken_) return false;
  syncDone_ = false;
  pendingSeq_ = pw_core_sync(core_, PW_ID_CORE, pendingSeq_);
  timespec abstime{};
  pw_thread_loop_get_time(loop_, &abstime, static_cast<int64_t>(timeoutMs) * SPA_NSEC_PER_MSEC);
  while (!syncDone_ && !broken_) {
    if (pw_thread_loop_timed_wait_full(loop_, &abstime) < 0) break;
  }
  return syncDone_ && !broken_;
}

void PwConnection::OnCoreInfo(void* data, const pw_core_info* info) {
  auto* self = static_cast<PwConnection*>(data);
  if (info->version != nullptr) self->serverVersion_ = info->version;
  if (info->name != nullptr) self->serverName_ = info->name;
}

void PwConnection::OnCoreDone(void* data, uint32_t id, int seq) {
  auto* self = static_cast<PwConnection*>(data);
  if (id == PW_ID_CORE && seq == self->pendingSeq_) {
    self->syncDone_ = true;
    pw_thread_loop_signal(self->loop_, false);
  }
}

void PwConnection::OnCoreError(void* data, uint32_t id, int seq, int res, const char* message) {
  (void)seq;
  auto* self = static_cast<PwConnection*>(data);
  if (id == PW_ID_CORE && res == -EPIPE) {
    self->broken_ = true;
    pw_thread_loop_signal(self->loop_, false);
  }
  if (self->errorFn_) self->errorFn_(id, res, message != nullptr ? message : spa_strerror(res));
}

}  // namespace plaud_rec
