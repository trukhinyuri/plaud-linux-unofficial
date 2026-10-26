// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// PipeWire connection shared by the meeting detector and the smart
// microphone/speaker monitors of one AudioMonitor instance.
//
// * One pw_thread_loop ("plaud-am-pw"), started by the first user and joined
//   by the last one (no thread exists while nothing is monitored).
// * Registry mirror: audio nodes (bound for props + state), links, clients
//   (bound for props: pipewire.sec.pid / application.*), and the "default"
//   metadata (default.audio.source / default.audio.sink).
// * The graph is reported as synced after two core roundtrips (globals, then
//   the info events of the bound proxies); later changes are coalesced and
//   delivered to observers on the loop thread.
// * A lost daemon connection (core error -EPIPE, e.g. PipeWire restarted)
//   tears everything down, tells observers, and reconnects with exponential
//   back-off (0.5 s .. 5 s); observers rebuild their streams on reconnect.
//
// Threading: every method except addUser/removeUser/lock/unlock must be
// called on the loop thread or with the loop lock held.
#pragma once

#include <pipewire/pipewire.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common.h"
#include "graph_types.h"

namespace am {

class GraphObserver {
 public:
  virtual ~GraphObserver() = default;
  // Coalesced graph change (also sent once right after each (re)connection).
  virtual void onGraphChanged() = 0;
  // The daemon connection is gone: destroy every pw_stream now (the core is
  // destroyed right after this returns).
  virtual void onConnectionLost() = 0;
};

class PwEngine {
 public:
  explicit PwEngine(LogFn log);
  ~PwEngine();
  PwEngine(const PwEngine&) = delete;
  PwEngine& operator=(const PwEngine&) = delete;

  // Reference-counted start/stop of the loop thread (call without the lock).
  void addUser();
  void removeUser();

  void lock();
  void unlock();
  // With the lock held: waits (releasing the lock) until the registry mirror
  // is synced or the timeout expires. Returns synced().
  bool waitSynced(int timeoutMs);

  bool connected() const { return core_ != nullptr; }
  bool synced() const { return synced_; }
  pw_core* core() const { return core_; }
  pw_loop* loop() const;

  void addObserver(GraphObserver* observer);
  void removeObserver(GraphObserver* observer);

  CaptureSnapshot captureSnapshot() const;
  // Nodes whose media.class starts with `classPrefix`, in creation order.
  std::vector<DeviceNode> devices(const std::string& classPrefix) const;
  const std::string& defaultSourceName() const { return defaultSource_; }
  const std::string& defaultSinkName() const { return defaultSink_; }

  // Loop timers (loop thread / lock held).
  spa_source* addTimer(std::function<void()> callback);
  void armTimer(spa_source* timer, int64_t initialMs, int64_t intervalMs);
  void destroyTimer(spa_source* timer);

  void log(const std::string& level, const std::string& message, const Props& meta = Props()) const;

 private:
  struct NodeEntry {
    PwEngine* engine = nullptr;
    uint32_t id = 0;
    pw_proxy* proxy = nullptr;
    spa_hook listener{};
    Props props;
    std::string mediaClass;
    pw_node_state state = PW_NODE_STATE_CREATING;
  };
  struct ClientEntry {
    PwEngine* engine = nullptr;
    uint32_t id = 0;
    pw_proxy* proxy = nullptr;
    spa_hook listener{};
    Props props;
  };
  struct LinkEntry {
    uint32_t outputNode = 0;
    uint32_t inputNode = 0;
  };
  struct TimerEntry {
    std::function<void()> callback;
  };

  void connect();
  void teardown();
  void scheduleReconnect();
  void scheduleNotify();
  void notifyObservers();
  void startSync();

  static void onCoreDone(void* data, uint32_t id, int seq);
  static void onCoreError(void* data, uint32_t id, int seq, int res, const char* message);
  static void onRegistryGlobal(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version,
                               const spa_dict* props);
  static void onRegistryGlobalRemove(void* data, uint32_t id);
  static void onNodeInfo(void* data, const pw_node_info* info);
  static void onClientInfo(void* data, const pw_client_info* info);
  static int onMetadataProperty(void* data, uint32_t subject, const char* key, const char* type, const char* value);
  static void onTimer(void* data, uint64_t expirations);
  static void onDisconnectEvent(void* data, uint64_t count);
  static void onReconnectTimer(void* data, uint64_t expirations);
  static void onNotifyTimer(void* data, uint64_t expirations);

  LogFn log_;
  int users_ = 0;
  pw_thread_loop* threadLoop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  spa_hook coreListener_{};
  pw_registry* registry_ = nullptr;
  spa_hook registryListener_{};
  pw_proxy* metadata_ = nullptr;
  uint32_t metadataId_ = 0;
  spa_hook metadataListener_{};

  spa_source* disconnectEvent_ = nullptr;
  spa_source* reconnectTimer_ = nullptr;
  spa_source* notifyTimer_ = nullptr;
  int64_t reconnectDelayMs_ = 500;
  bool notifyPending_ = false;
  bool everConnected_ = false;
  bool reportedDown_ = false;

  int syncSeq_ = 0;
  int syncStage_ = 0;
  bool synced_ = false;

  std::map<uint32_t, std::unique_ptr<NodeEntry>> nodes_;
  std::map<uint32_t, std::unique_ptr<ClientEntry>> clients_;
  std::map<uint32_t, LinkEntry> links_;
  std::string defaultSource_;
  std::string defaultSink_;
  std::vector<GraphObserver*> observers_;
  std::map<spa_source*, std::unique_ptr<TimerEntry>> timers_;
};

// RAII helper for the engine lock.
class PwLock {
 public:
  explicit PwLock(PwEngine& engine) : engine_(engine) { engine_.lock(); }
  ~PwLock() { engine_.unlock(); }
  PwLock(const PwLock&) = delete;
  PwLock& operator=(const PwLock&) = delete;

 private:
  PwEngine& engine_;
};

}  // namespace am
