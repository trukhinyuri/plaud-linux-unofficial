// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// One PipeWire client connection: thread loop + context + core, with a
// synchronous round-trip helper and core-error (daemon gone) reporting.

#pragma once

#include <pipewire/pipewire.h>

#include <functional>
#include <string>

namespace plaud_rec {

// Initializes libpipewire once per process and keeps its own log output
// quiet unless PIPEWIRE_DEBUG is set; the addon reports through JS callbacks.
void EnsurePipeWireInit();

class PwConnection {
 public:
  using ErrorFn = std::function<void(uint32_t id, int res, const std::string& message)>;

  explicit PwConnection(std::string threadName);
  ~PwConnection();
  PwConnection(const PwConnection&) = delete;
  PwConnection& operator=(const PwConnection&) = delete;

  // Creates and starts the loop thread, connects to the daemon and performs a
  // round trip. On failure everything is torn down and *error describes why.
  bool Connect(int timeoutMs, std::string* error);
  // Stops the loop thread and releases the connection. Proxies/streams created
  // on this core must have been destroyed (with the lock held) beforehand.
  void Disconnect();

  bool connected() const { return core_ != nullptr && !broken_; }
  bool broken() const { return broken_; }
  pw_thread_loop* threadLoop() const { return loop_; }
  pw_loop* loop() const { return loop_ != nullptr ? pw_thread_loop_get_loop(loop_) : nullptr; }
  pw_context* context() const { return context_; }
  pw_core* core() const { return core_; }
  bool InLoopThread() const { return loop_ != nullptr && pw_thread_loop_in_thread(loop_); }
  const std::string& serverVersion() const { return serverVersion_; }
  const std::string& serverName() const { return serverName_; }

  void Lock() const { pw_thread_loop_lock(loop_); }
  void Unlock() const { pw_thread_loop_unlock(loop_); }

  // Round trip with the server; the caller must hold the loop lock and must
  // not be on the loop thread. Returns false on timeout or broken connection.
  bool SyncLocked(int timeoutMs);

  // Invoked on the loop thread (lock held) for core errors. id == PW_ID_CORE
  // with res == -EPIPE means the daemon connection is gone.
  void SetErrorHandler(ErrorFn fn) { errorFn_ = std::move(fn); }

 private:
  static void OnCoreInfo(void* data, const pw_core_info* info);
  static void OnCoreDone(void* data, uint32_t id, int seq);
  static void OnCoreError(void* data, uint32_t id, int seq, int res, const char* message);

  std::string threadName_;
  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  spa_hook coreListener_{};
  bool listenerAdded_ = false;
  int pendingSeq_ = 0;
  bool syncDone_ = false;
  bool broken_ = false;
  std::string serverVersion_;
  std::string serverName_;
  ErrorFn errorFn_;
};

// RAII lock of a connection's thread loop.
class PwLock {
 public:
  explicit PwLock(const PwConnection& c) : c_(c) { c_.Lock(); }
  ~PwLock() { c_.Unlock(); }
  PwLock(const PwLock&) = delete;
  PwLock& operator=(const PwLock&) = delete;

 private:
  const PwConnection& c_;
};

}  // namespace plaud_rec
