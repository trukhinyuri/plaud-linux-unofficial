// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "pw_engine.h"

#include <pipewire/extensions/metadata.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

#include "json.h"
#include "proc_util.h"

namespace am {

namespace {

Props dictToProps(const spa_dict* dict) {
  Props out;
  if (dict == nullptr) return out;
  const spa_dict_item* item;
  spa_dict_for_each(item, dict) {
    if (item->key != nullptr) out[item->key] = item->value != nullptr ? item->value : "";
  }
  return out;
}

uint64_t serialOf(const Props& props, uint32_t fallback) {
  const std::string s = prop(props, "object.serial");
  if (!s.empty()) {
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end != s.c_str()) return static_cast<uint64_t>(v);
  }
  return fallback;
}

uint32_t u32Of(const std::string& s) {
  if (s.empty()) return SPA_ID_INVALID;
  char* end = nullptr;
  unsigned long v = std::strtoul(s.c_str(), &end, 10);
  return end == s.c_str() ? SPA_ID_INVALID : static_cast<uint32_t>(v);
}

bool isTrackedNodeClass(const std::string& mediaClass) {
  return startsWith(mediaClass, "Audio/") || mediaClass == "Stream/Input/Audio";
}

bool isMicClass(const std::string& mediaClass) {
  return startsWith(mediaClass, "Audio/Source") || mediaClass == "Audio/Duplex";
}

std::string defaultNameFromValue(const char* value) {
  if (value == nullptr) return std::string();
  JsonValue v;
  if (!parseJson(value, v)) return std::string();
  return v.isObject() ? v.stringOr("name") : std::string();
}

timespec msToTimespec(int64_t ms) {
  timespec ts{};
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000;
  return ts;
}

}  // namespace

// Event tables (static storage: PipeWire keeps the pointers).
static pw_core_events gCoreEvents;
static pw_registry_events gRegistryEvents;
static pw_node_events gNodeEvents;
static pw_client_events gClientEvents;
static pw_metadata_events gMetadataEvents;

PwEngine::PwEngine(LogFn log) : log_(std::move(log)) {
  static std::once_flag once;
  std::call_once(once, [] {
    pw_init(nullptr, nullptr);
    gCoreEvents.version = PW_VERSION_CORE_EVENTS;
    gCoreEvents.done = &PwEngine::onCoreDone;
    gCoreEvents.error = &PwEngine::onCoreError;
    gRegistryEvents.version = PW_VERSION_REGISTRY_EVENTS;
    gRegistryEvents.global = &PwEngine::onRegistryGlobal;
    gRegistryEvents.global_remove = &PwEngine::onRegistryGlobalRemove;
    gNodeEvents.version = PW_VERSION_NODE_EVENTS;
    gNodeEvents.info = &PwEngine::onNodeInfo;
    gClientEvents.version = PW_VERSION_CLIENT_EVENTS;
    gClientEvents.info = &PwEngine::onClientInfo;
    gMetadataEvents.version = PW_VERSION_METADATA_EVENTS;
    gMetadataEvents.property = &PwEngine::onMetadataProperty;
  });
}

PwEngine::~PwEngine() {
  while (users_ > 0) removeUser();
}

void PwEngine::log(const std::string& level, const std::string& message, const Props& meta) const {
  if (log_) log_(level, message, meta);
}

pw_loop* PwEngine::loop() const { return threadLoop_ != nullptr ? pw_thread_loop_get_loop(threadLoop_) : nullptr; }

void PwEngine::lock() { pw_thread_loop_lock(threadLoop_); }
void PwEngine::unlock() { pw_thread_loop_unlock(threadLoop_); }

void PwEngine::addUser() {
  if (users_++ > 0) return;
  threadLoop_ = pw_thread_loop_new("plaud-am-pw", nullptr);
  pw_loop* l = pw_thread_loop_get_loop(threadLoop_);
  context_ = pw_context_new(l,
                            pw_properties_new(PW_KEY_APP_NAME, "Plaud audio monitor", PW_KEY_APP_ID,
                                              "ai.plaud.desktop.audio-monitor", nullptr),
                            0);
  disconnectEvent_ = pw_loop_add_event(l, &PwEngine::onDisconnectEvent, this);
  reconnectTimer_ = pw_loop_add_timer(l, &PwEngine::onReconnectTimer, this);
  notifyTimer_ = pw_loop_add_timer(l, &PwEngine::onNotifyTimer, this);
  pw_thread_loop_start(threadLoop_);
  lock();
  log("info", "Starting PipeWire connection",
      {{"libraryVersion", pw_get_library_version()}, {"headersVersion", pw_get_headers_version()}});
  connect();
  unlock();
}

void PwEngine::removeUser() {
  if (users_ == 0) return;
  if (--users_ > 0) return;
  lock();
  observers_.clear();
  teardown();
  pw_loop* l = pw_thread_loop_get_loop(threadLoop_);
  for (auto& kv : timers_) pw_loop_destroy_source(l, kv.first);
  timers_.clear();
  pw_loop_destroy_source(l, disconnectEvent_);
  pw_loop_destroy_source(l, reconnectTimer_);
  pw_loop_destroy_source(l, notifyTimer_);
  disconnectEvent_ = reconnectTimer_ = notifyTimer_ = nullptr;
  unlock();
  pw_thread_loop_stop(threadLoop_);
  pw_context_destroy(context_);
  pw_thread_loop_destroy(threadLoop_);
  context_ = nullptr;
  threadLoop_ = nullptr;
  notifyPending_ = false;
  reconnectDelayMs_ = 500;
  everConnected_ = false;
  reportedDown_ = false;
}

bool PwEngine::waitSynced(int timeoutMs) {
  if (synced_) return true;
  timespec abstime{};
  pw_thread_loop_get_time(threadLoop_, &abstime, static_cast<int64_t>(timeoutMs) * SPA_NSEC_PER_MSEC);
  while (!synced_) {
    if (pw_thread_loop_timed_wait_full(threadLoop_, &abstime) != 0) break;
  }
  return synced_;
}

void PwEngine::connect() {
  if (core_ != nullptr || context_ == nullptr) return;
  core_ = pw_context_connect(context_, nullptr, 0);
  if (core_ == nullptr) {
    const int err = errno;
    if (!reportedDown_) {
      reportedDown_ = true;
      log("warn", std::string("PipeWire connection failed: ") + std::strerror(err) + ", retrying");
    }
    scheduleReconnect();
    return;
  }
  pw_core_add_listener(core_, &coreListener_, &gCoreEvents, this);
  registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(registry_, &registryListener_, &gRegistryEvents, this);
  startSync();
}

void PwEngine::startSync() {
  synced_ = false;
  syncStage_ = 1;
  syncSeq_ = pw_core_sync(core_, PW_ID_CORE, syncSeq_);
}

void PwEngine::teardown() {
  std::vector<GraphObserver*> observers = observers_;
  for (GraphObserver* o : observers) o->onConnectionLost();
  if (metadata_ != nullptr) {
    spa_hook_remove(&metadataListener_);
    pw_proxy_destroy(metadata_);
    metadata_ = nullptr;
    metadataId_ = 0;
  }
  for (auto& kv : nodes_) {
    if (kv.second->proxy != nullptr) {
      spa_hook_remove(&kv.second->listener);
      pw_proxy_destroy(kv.second->proxy);
    }
  }
  for (auto& kv : clients_) {
    if (kv.second->proxy != nullptr) {
      spa_hook_remove(&kv.second->listener);
      pw_proxy_destroy(kv.second->proxy);
    }
  }
  nodes_.clear();
  clients_.clear();
  links_.clear();
  defaultSource_.clear();
  defaultSink_.clear();
  if (registry_ != nullptr) {
    spa_hook_remove(&registryListener_);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
    registry_ = nullptr;
  }
  if (core_ != nullptr) {
    spa_hook_remove(&coreListener_);
    pw_core_disconnect(core_);
    core_ = nullptr;
  }
  synced_ = false;
  syncStage_ = 0;
  if (threadLoop_ != nullptr) pw_thread_loop_signal(threadLoop_, false);
}

void PwEngine::scheduleReconnect() {
  armTimer(reconnectTimer_, reconnectDelayMs_, 0);
  reconnectDelayMs_ = std::min<int64_t>(reconnectDelayMs_ * 2, 5000);
}

void PwEngine::scheduleNotify() {
  if (!synced_ || notifyPending_) return;
  notifyPending_ = true;
  armTimer(notifyTimer_, 20, 0);
}

void PwEngine::notifyObservers() {
  std::vector<GraphObserver*> observers = observers_;
  for (GraphObserver* o : observers) {
    if (std::find(observers_.begin(), observers_.end(), o) != observers_.end()) o->onGraphChanged();
  }
}

void PwEngine::addObserver(GraphObserver* observer) {
  if (std::find(observers_.begin(), observers_.end(), observer) == observers_.end()) observers_.push_back(observer);
}

void PwEngine::removeObserver(GraphObserver* observer) {
  observers_.erase(std::remove(observers_.begin(), observers_.end(), observer), observers_.end());
}

spa_source* PwEngine::addTimer(std::function<void()> callback) {
  std::unique_ptr<TimerEntry> entry(new TimerEntry{std::move(callback)});
  spa_source* source = pw_loop_add_timer(loop(), &PwEngine::onTimer, entry.get());
  if (source != nullptr) timers_[source] = std::move(entry);
  return source;
}

void PwEngine::armTimer(spa_source* timer, int64_t initialMs, int64_t intervalMs) {
  if (timer == nullptr) return;
  timespec value = msToTimespec(initialMs);
  timespec interval = msToTimespec(intervalMs);
  pw_loop_update_timer(loop(), timer, &value, intervalMs > 0 ? &interval : nullptr, false);
}

void PwEngine::destroyTimer(spa_source* timer) {
  if (timer == nullptr) return;
  pw_loop_destroy_source(loop(), timer);
  timers_.erase(timer);
}

CaptureSnapshot PwEngine::captureSnapshot() const {
  CaptureSnapshot snap;
  snap.connected = core_ != nullptr && synced_;
  for (const auto& kv : nodes_) {
    const NodeEntry& n = *kv.second;
    if (n.mediaClass != "Stream/Input/Audio") continue;
    CaptureStream s;
    s.nodeId = n.id;
    s.serial = serialOf(n.props, n.id);
    s.nodeProps = n.props;
    auto client = clients_.find(u32Of(prop(n.props, "client.id")));
    if (client != clients_.end()) s.clientProps = client->second->props;
    s.running = n.state == PW_NODE_STATE_RUNNING;
    s.corked = prop(n.props, "pulse.corked") == "true";
    s.monitorOnly = prop(n.props, "stream.monitor") == "true";
    for (const auto& lk : links_) {
      if (lk.second.inputNode != n.id) continue;
      auto src = nodes_.find(lk.second.outputNode);
      if (src == nodes_.end() || !isMicClass(src->second->mediaClass)) continue;
      s.micLinked = true;
      s.sourceName = prop(src->second->props, "node.name");
      for (const char* key : {"node.description", "node.nick", "node.name"}) {
        s.sourceDescription = prop(src->second->props, key);
        if (!s.sourceDescription.empty()) break;
      }
      break;
    }
    snap.streams.push_back(std::move(s));
  }
  return snap;
}

std::vector<DeviceNode> PwEngine::devices(const std::string& classPrefix) const {
  std::vector<DeviceNode> out;
  for (const auto& kv : nodes_) {
    const NodeEntry& n = *kv.second;
    if (!startsWith(n.mediaClass, classPrefix)) continue;
    DeviceNode d;
    d.id = n.id;
    d.serial = serialOf(n.props, n.id);
    d.name = prop(n.props, "node.name");
    for (const char* key : {"node.description", "node.nick", "node.name"}) {
      d.description = prop(n.props, key);
      if (!d.description.empty()) break;
    }
    d.mediaClass = n.mediaClass;
    d.props = n.props;
    if (!d.name.empty()) out.push_back(std::move(d));
  }
  // Creation order (object.serial never repeats, unlike reused node ids), the
  // closest equivalent of the CoreAudio / MMDevice enumeration order.
  std::sort(out.begin(), out.end(), [](const DeviceNode& a, const DeviceNode& b) { return a.serial < b.serial; });
  return out;
}

// --- PipeWire callbacks (loop thread) -----------------------------------------

void PwEngine::onCoreDone(void* data, uint32_t id, int seq) {
  auto* self = static_cast<PwEngine*>(data);
  if (id != PW_ID_CORE || seq != self->syncSeq_ || self->syncStage_ == 0) return;
  if (self->syncStage_ == 1) {
    self->syncStage_ = 2;
    self->syncSeq_ = pw_core_sync(self->core_, PW_ID_CORE, self->syncSeq_);
    return;
  }
  self->syncStage_ = 0;
  self->synced_ = true;
  self->reconnectDelayMs_ = 500;
  const bool reconnected = self->everConnected_;
  self->everConnected_ = true;
  self->reportedDown_ = false;
  self->log("info", reconnected ? "Reconnected to PipeWire" : "Connected to PipeWire",
            {{"nodes", std::to_string(self->nodes_.size())}, {"clients", std::to_string(self->clients_.size())}});
  pw_thread_loop_signal(self->threadLoop_, false);
  self->notifyObservers();
}

void PwEngine::onCoreError(void* data, uint32_t id, int seq, int res, const char* message) {
  auto* self = static_cast<PwEngine*>(data);
  if (id != PW_ID_CORE) {
    self->log("debug", std::string("PipeWire object error: ") + (message ? message : ""),
              {{"id", std::to_string(id)}, {"res", std::to_string(res)}});
    return;
  }
  if (res == -EPIPE) {
    pw_loop_signal_event(self->loop(), self->disconnectEvent_);
    return;
  }
  self->log("warn", std::string("PipeWire core error: ") + (message ? message : ""), {{"res", std::to_string(res)}});
}

void PwEngine::onDisconnectEvent(void* data, uint64_t) {
  auto* self = static_cast<PwEngine*>(data);
  if (self->core_ == nullptr) return;
  self->log("warn", "PipeWire connection lost, reconnecting");
  self->reportedDown_ = true;
  self->teardown();
  self->notifyObservers();
  self->scheduleReconnect();
}

void PwEngine::onReconnectTimer(void* data, uint64_t) {
  auto* self = static_cast<PwEngine*>(data);
  if (self->core_ == nullptr) self->connect();
}

void PwEngine::onNotifyTimer(void* data, uint64_t) {
  auto* self = static_cast<PwEngine*>(data);
  self->notifyPending_ = false;
  self->notifyObservers();
}

void PwEngine::onTimer(void* data, uint64_t) {
  auto* entry = static_cast<TimerEntry*>(data);
  if (entry->callback) entry->callback();
}

void PwEngine::onRegistryGlobal(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version,
                                const spa_dict* props) {
  auto* self = static_cast<PwEngine*>(data);
  if (type == nullptr) return;
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    Props p = dictToProps(props);
    const std::string mediaClass = prop(p, "media.class");
    if (!isTrackedNodeClass(mediaClass)) return;
    std::unique_ptr<NodeEntry> entry(new NodeEntry());
    entry->engine = self;
    entry->id = id;
    entry->props = std::move(p);
    entry->mediaClass = mediaClass;
    entry->proxy = static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, type, PW_VERSION_NODE, 0));
    if (entry->proxy != nullptr) {
      pw_node_add_listener(reinterpret_cast<pw_node*>(entry->proxy), &entry->listener, &gNodeEvents, entry.get());
    }
    self->nodes_[id] = std::move(entry);
    self->scheduleNotify();
  } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
    Props p = dictToProps(props);
    LinkEntry link;
    link.outputNode = u32Of(prop(p, "link.output.node"));
    link.inputNode = u32Of(prop(p, "link.input.node"));
    self->links_[id] = link;
    self->scheduleNotify();
  } else if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
    std::unique_ptr<ClientEntry> entry(new ClientEntry());
    entry->engine = self;
    entry->id = id;
    entry->props = dictToProps(props);
    entry->proxy = static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, type, PW_VERSION_CLIENT, 0));
    if (entry->proxy != nullptr) {
      pw_client_add_listener(reinterpret_cast<pw_client*>(entry->proxy), &entry->listener, &gClientEvents,
                             entry.get());
    }
    self->clients_[id] = std::move(entry);
  } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
    const char* name = props != nullptr ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : nullptr;
    if (name == nullptr || std::strcmp(name, "default") != 0 || self->metadata_ != nullptr) return;
    self->metadata_ = static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, type, PW_VERSION_METADATA, 0));
    if (self->metadata_ != nullptr) {
      self->metadataId_ = id;
      pw_metadata_add_listener(reinterpret_cast<pw_metadata*>(self->metadata_), &self->metadataListener_,
                               &gMetadataEvents, self);
    }
  }
}

void PwEngine::onRegistryGlobalRemove(void* data, uint32_t id) {
  auto* self = static_cast<PwEngine*>(data);
  auto node = self->nodes_.find(id);
  if (node != self->nodes_.end()) {
    if (node->second->proxy != nullptr) {
      spa_hook_remove(&node->second->listener);
      pw_proxy_destroy(node->second->proxy);
    }
    self->nodes_.erase(node);
    self->scheduleNotify();
    return;
  }
  if (self->links_.erase(id) > 0) {
    self->scheduleNotify();
    return;
  }
  auto client = self->clients_.find(id);
  if (client != self->clients_.end()) {
    if (client->second->proxy != nullptr) {
      spa_hook_remove(&client->second->listener);
      pw_proxy_destroy(client->second->proxy);
    }
    self->clients_.erase(client);
    return;
  }
  if (self->metadata_ != nullptr && id == self->metadataId_) {
    spa_hook_remove(&self->metadataListener_);
    pw_proxy_destroy(self->metadata_);
    self->metadata_ = nullptr;
    self->metadataId_ = 0;
    self->defaultSource_.clear();
    self->defaultSink_.clear();
    self->scheduleNotify();
  }
}

void PwEngine::onNodeInfo(void* data, const pw_node_info* info) {
  auto* entry = static_cast<NodeEntry*>(data);
  bool changed = false;
  if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0 && info->props != nullptr) {
    Props p = dictToProps(info->props);
    if (p != entry->props) {
      const std::string mediaClass = prop(p, "media.class");
      if (!mediaClass.empty()) entry->mediaClass = mediaClass;
      entry->props = std::move(p);
      changed = true;
    }
  }
  if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) != 0 && entry->state != info->state) {
    entry->state = info->state;
    changed = true;
  }
  if (changed) entry->engine->scheduleNotify();
}

void PwEngine::onClientInfo(void* data, const pw_client_info* info) {
  auto* entry = static_cast<ClientEntry*>(data);
  if ((info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS) != 0 && info->props != nullptr) {
    entry->props = dictToProps(info->props);
    entry->engine->scheduleNotify();
  }
}

int PwEngine::onMetadataProperty(void* data, uint32_t subject, const char* key, const char* type, const char* value) {
  auto* self = static_cast<PwEngine*>(data);
  if (subject != PW_ID_CORE) return 0;
  if (key == nullptr) {
    self->defaultSource_.clear();
    self->defaultSink_.clear();
  } else if (std::strcmp(key, "default.audio.source") == 0) {
    self->defaultSource_ = defaultNameFromValue(value);
  } else if (std::strcmp(key, "default.audio.sink") == 0) {
    self->defaultSink_ = defaultNameFromValue(value);
  } else {
    return 0;
  }
  self->scheduleNotify();
  return 0;
}

}  // namespace am
