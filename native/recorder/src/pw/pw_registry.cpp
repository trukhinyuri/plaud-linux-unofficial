// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "pw/pw_registry.h"

#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/raw.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/iter.h>
#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/utils/json.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace plaud_rec {

namespace {

const char* DictGet(const spa_dict* dict, const char* key) {
  if (dict == nullptr) return nullptr;
  return spa_dict_lookup(dict, key);
}

std::string Str(const char* s) { return s != nullptr ? std::string(s) : std::string(); }

int ParseInt(const char* s, int fallback) {
  if (s == nullptr || *s == '\0') return fallback;
  char* end = nullptr;
  const long v = std::strtol(s, &end, 10);
  return (end != s) ? static_cast<int>(v) : fallback;
}

uint64_t ParseU64(const char* s) {
  if (s == nullptr) return 0;
  return std::strtoull(s, nullptr, 10);
}

bool IsAudioSourceClass(const std::string& c) {
  return c == "Audio/Source" || c == "Audio/Source/Virtual" || c == "Audio/Duplex";
}

bool IsAudioSinkClass(const std::string& c) { return c == "Audio/Sink" || c == "Audio/Duplex"; }

// Metadata values look like {"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"}.
std::string ParseMetadataName(const char* value) {
  if (value == nullptr) return {};
  spa_json it[2];
  spa_json_init(&it[0], value, std::strlen(value));
  if (spa_json_enter_object(&it[0], &it[1]) <= 0) return {};
  char key[64];
  while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
    if (std::strcmp(key, "name") == 0) {
      char name[1024];
      if (spa_json_get_string(&it[1], name, sizeof(name)) > 0) return name;
      return {};
    }
    const char* skip = nullptr;
    if (spa_json_next(&it[1], &skip) <= 0) break;
  }
  return {};
}

void ApplyNodeProps(PwAudioNode& n, const spa_dict* props) {
  if (props == nullptr) return;
  if (const char* v = DictGet(props, PW_KEY_NODE_NAME)) n.name = v;
  if (const char* v = DictGet(props, PW_KEY_NODE_DESCRIPTION)) n.description = v;
  if (const char* v = DictGet(props, PW_KEY_NODE_NICK)) n.nick = v;
  if (const char* v = DictGet(props, PW_KEY_MEDIA_CLASS)) n.mediaClass = v;
  if (const char* v = DictGet(props, PW_KEY_DEVICE_ID)) n.deviceId = static_cast<uint32_t>(ParseU64(v));
  if (const char* v = DictGet(props, PW_KEY_DEVICE_FORM_FACTOR)) n.formFactor = v;
  if (const char* v = DictGet(props, PW_KEY_DEVICE_BUS)) n.bus = v;
  if (const char* v = DictGet(props, PW_KEY_PRIORITY_SESSION)) n.prioritySession = ParseInt(v, 0);
  if (const char* v = DictGet(props, "card.profile.device")) n.profileDevice = ParseInt(v, -1);
  if (const char* v = DictGet(props, "object.serial")) n.serial = ParseU64(v);
  n.isSource = IsAudioSourceClass(n.mediaClass);
  n.isSink = IsAudioSinkClass(n.mediaClass);
}

void ApplyDeviceProps(PwAudioDevice& d, const spa_dict* props) {
  if (props == nullptr) return;
  if (const char* v = DictGet(props, PW_KEY_DEVICE_NAME)) d.name = v;
  if (const char* v = DictGet(props, PW_KEY_DEVICE_DESCRIPTION)) d.description = v;
  if (const char* v = DictGet(props, PW_KEY_DEVICE_FORM_FACTOR)) d.formFactor = v;
  if (const char* v = DictGet(props, PW_KEY_DEVICE_BUS)) d.bus = v;
  if (const char* v = DictGet(props, PW_KEY_DEVICE_API)) d.api = v;
}

bool ParseRoute(const spa_pod* param, uint32_t* direction, PwRoute* route) {
  if (param == nullptr || !spa_pod_is_object_type(param, SPA_TYPE_OBJECT_ParamRoute)) return false;
  auto* obj = reinterpret_cast<spa_pod_object*>(const_cast<spa_pod*>(param));
  bool haveDirection = false;
  spa_pod_prop* prop;
  SPA_POD_OBJECT_FOREACH(obj, prop) {
    switch (prop->key) {
      case SPA_PARAM_ROUTE_direction:
        haveDirection = spa_pod_get_id(&prop->value, direction) == 0;
        break;
      case SPA_PARAM_ROUTE_device: {
        int32_t dev = -1;
        if (spa_pod_get_int(&prop->value, &dev) == 0) route->device = dev;
        break;
      }
      case SPA_PARAM_ROUTE_name: {
        const char* s = nullptr;
        if (spa_pod_get_string(&prop->value, &s) == 0 && s != nullptr) route->name = s;
        break;
      }
      case SPA_PARAM_ROUTE_description: {
        const char* s = nullptr;
        if (spa_pod_get_string(&prop->value, &s) == 0 && s != nullptr) route->description = s;
        break;
      }
      case SPA_PARAM_ROUTE_info: {
        // Struct: Int n, then n (String key, String value) pairs.
        if (!spa_pod_is_struct(&prop->value)) break;
        // Explicit loop instead of SPA_POD_STRUCT_FOREACH: before PipeWire 1.2 the macro assigns
        // the void* from SPA_POD_BODY/spa_pod_next to the iterator, which C++ rejects.
        void* body = SPA_POD_BODY(&prop->value);
        const uint32_t bodySize = SPA_POD_BODY_SIZE(&prop->value);
        const char* key = nullptr;
        bool first = true;
        for (auto* field = static_cast<spa_pod*>(body); spa_pod_is_inside(body, bodySize, field);
             field = static_cast<spa_pod*>(spa_pod_next(field))) {
          if (first) {
            first = false;
            continue;
          }
          const char* s = nullptr;
          if (spa_pod_get_string(field, &s) != 0) continue;
          if (key == nullptr) {
            key = s;
          } else {
            if (key != nullptr && s != nullptr && std::strcmp(key, "port.type") == 0) route->portType = s;
            key = nullptr;
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return haveDirection;
}

void ParseProps(const spa_pod* param, PwAudioNode& n) {
  if (param == nullptr || !spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props)) return;
  auto* obj = reinterpret_cast<spa_pod_object*>(const_cast<spa_pod*>(param));
  spa_pod_prop* prop;
  SPA_POD_OBJECT_FOREACH(obj, prop) {
    switch (prop->key) {
      case SPA_PROP_mute: {
        bool mute = false;
        if (spa_pod_get_bool(&prop->value, &mute) == 0) {
          n.mute = mute;
          n.propsKnown = true;
        }
        break;
      }
      case SPA_PROP_channelVolumes: {
        float vols[SPA_AUDIO_MAX_CHANNELS];
        const uint32_t count = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, vols, SPA_AUDIO_MAX_CHANNELS);
        if (count > 0) {
          float maxVol = 0.f;
          for (uint32_t i = 0; i < count; ++i) maxVol = std::max(maxVol, vols[i]);
          n.maxVolume = maxVol;
          n.propsKnown = true;
        }
        break;
      }
      default:
        break;
    }
  }
}

pw_registry_events MakeRegistryEvents() {
  pw_registry_events ev{};
  ev.version = PW_VERSION_REGISTRY_EVENTS;
  return ev;
}

}  // namespace

struct PwRegistry::Bound {
  enum class Kind { Node, Device, Metadata };
  PwRegistry* owner = nullptr;
  uint32_t id = 0;
  Kind kind = Kind::Node;
  pw_proxy* proxy = nullptr;
  spa_hook objectListener{};
  spa_hook proxyListener{};
  bool objectListenerAdded = false;
  bool proxyListenerAdded = false;
};

PwRegistry::PwRegistry(PwConnection* connection) : conn_(connection) {}

PwRegistry::~PwRegistry() { Stop(); }

bool PwRegistry::Start(ChangeFn onChange) {
  static pw_registry_events kEvents = [] {
    pw_registry_events ev = MakeRegistryEvents();
    ev.global = &PwRegistry::OnGlobal;
    ev.global_remove = &PwRegistry::OnGlobalRemove;
    return ev;
  }();
  if (conn_ == nullptr || conn_->core() == nullptr) return false;
  onChange_ = std::move(onChange);
  registry_ = pw_core_get_registry(conn_->core(), PW_VERSION_REGISTRY, 0);
  if (registry_ == nullptr) return false;
  pw_registry_add_listener(registry_, &registryListener_, &kEvents, this);
  return true;
}

void PwRegistry::Stop() {
  for (auto& it : bound_) DestroyBound(it.second.get());
  bound_.clear();
  if (registry_ != nullptr) {
    spa_hook_remove(&registryListener_);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
    registry_ = nullptr;
  }
  onChange_ = nullptr;
  std::lock_guard<std::mutex> lock(mu_);
  nodes_.clear();
  devices_.clear();
  links_.clear();
  defaultSource_.clear();
  defaultSink_.clear();
  metadataId_ = 0xffffffffu;
}

void PwRegistry::DestroyBound(Bound* b) {
  if (b->objectListenerAdded) {
    spa_hook_remove(&b->objectListener);
    b->objectListenerAdded = false;
  }
  if (b->proxyListenerAdded) {
    spa_hook_remove(&b->proxyListener);
    b->proxyListenerAdded = false;
  }
  if (b->proxy != nullptr) {
    pw_proxy_destroy(b->proxy);
    b->proxy = nullptr;
  }
}

void PwRegistry::Notify(uint32_t mask) {
  if (onChange_) onChange_(mask);
}

void PwRegistry::OnGlobal(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version,
                          const spa_dict* props) {
  (void)permissions;
  (void)version;
  static pw_node_events kNodeEvents = [] {
    pw_node_events ev{};
    ev.version = PW_VERSION_NODE_EVENTS;
    ev.info = &PwRegistry::OnNodeInfo;
    ev.param = &PwRegistry::OnNodeParam;
    return ev;
  }();
  static pw_device_events kDeviceEvents = [] {
    pw_device_events ev{};
    ev.version = PW_VERSION_DEVICE_EVENTS;
    ev.info = &PwRegistry::OnDeviceInfo;
    ev.param = &PwRegistry::OnDeviceParam;
    return ev;
  }();
  static pw_metadata_events kMetadataEvents = [] {
    pw_metadata_events ev{};
    ev.version = PW_VERSION_METADATA_EVENTS;
    ev.property = &PwRegistry::OnMetadataProperty;
    return ev;
  }();
  static pw_proxy_events kProxyEvents = [] {
    pw_proxy_events ev{};
    ev.version = PW_VERSION_PROXY_EVENTS;
    ev.destroy = &PwRegistry::OnProxyDestroy;
    return ev;
  }();

  auto* self = static_cast<PwRegistry*>(data);
  if (type == nullptr) return;

  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    const std::string mediaClass = Str(DictGet(props, PW_KEY_MEDIA_CLASS));
    if (!IsAudioSourceClass(mediaClass) && !IsAudioSinkClass(mediaClass)) return;
    PwAudioNode node;
    node.id = id;
    ApplyNodeProps(node, props);
    {
      std::lock_guard<std::mutex> lock(self->mu_);
      self->nodes_[id] = node;
    }
    auto b = std::make_unique<Bound>();
    b->owner = self;
    b->id = id;
    b->kind = Bound::Kind::Node;
    b->proxy = static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (b->proxy != nullptr) {
      pw_proxy_add_object_listener(b->proxy, &b->objectListener, &kNodeEvents, b.get());
      b->objectListenerAdded = true;
      pw_proxy_add_listener(b->proxy, &b->proxyListener, &kProxyEvents, b.get());
      b->proxyListenerAdded = true;
      uint32_t ids[1] = {SPA_PARAM_Props};
      pw_node_subscribe_params(reinterpret_cast<pw_node*>(b->proxy), ids, 1);
      self->bound_[id] = std::move(b);
    }
    self->Notify(kNodes);
    return;
  }

  if (std::strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
    if (Str(DictGet(props, PW_KEY_MEDIA_CLASS)) != "Audio/Device") return;
    PwAudioDevice dev;
    dev.id = id;
    ApplyDeviceProps(dev, props);
    {
      std::lock_guard<std::mutex> lock(self->mu_);
      self->devices_[id] = dev;
    }
    auto b = std::make_unique<Bound>();
    b->owner = self;
    b->id = id;
    b->kind = Bound::Kind::Device;
    b->proxy =
        static_cast<pw_proxy*>(pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0));
    if (b->proxy != nullptr) {
      pw_proxy_add_object_listener(b->proxy, &b->objectListener, &kDeviceEvents, b.get());
      b->objectListenerAdded = true;
      pw_proxy_add_listener(b->proxy, &b->proxyListener, &kProxyEvents, b.get());
      b->proxyListenerAdded = true;
      uint32_t ids[1] = {SPA_PARAM_Route};
      pw_device_subscribe_params(reinterpret_cast<pw_device*>(b->proxy), ids, 1);
      self->bound_[id] = std::move(b);
    }
    self->Notify(kDevices);
    return;
  }

  if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
    PwLinkInfo link;
    link.id = id;
    link.outputNode = static_cast<uint32_t>(ParseU64(DictGet(props, PW_KEY_LINK_OUTPUT_NODE)));
    link.inputNode = static_cast<uint32_t>(ParseU64(DictGet(props, PW_KEY_LINK_INPUT_NODE)));
    {
      std::lock_guard<std::mutex> lock(self->mu_);
      self->links_[id] = link;
    }
    self->Notify(kLinks);
    return;
  }

  if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
    if (Str(DictGet(props, PW_KEY_METADATA_NAME)) != "default") return;
    if (self->metadataId_ != 0xffffffffu) return;
    auto b = std::make_unique<Bound>();
    b->owner = self;
    b->id = id;
    b->kind = Bound::Kind::Metadata;
    b->proxy = static_cast<pw_proxy*>(
        pw_registry_bind(self->registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (b->proxy != nullptr) {
      pw_proxy_add_object_listener(b->proxy, &b->objectListener, &kMetadataEvents, b.get());
      b->objectListenerAdded = true;
      pw_proxy_add_listener(b->proxy, &b->proxyListener, &kProxyEvents, b.get());
      b->proxyListenerAdded = true;
      self->metadataId_ = id;
      self->bound_[id] = std::move(b);
    }
    return;
  }
}

void PwRegistry::OnGlobalRemove(void* data, uint32_t id) {
  auto* self = static_cast<PwRegistry*>(data);
  uint32_t mask = 0;
  {
    std::lock_guard<std::mutex> lock(self->mu_);
    if (self->nodes_.erase(id) > 0) mask |= kNodes;
    if (self->devices_.erase(id) > 0) mask |= kDevices;
    if (self->links_.erase(id) > 0) mask |= kLinks;
    if (id == self->metadataId_) {
      self->defaultSource_.clear();
      self->defaultSink_.clear();
      mask |= kDefaults;
    }
  }
  if (id == self->metadataId_) self->metadataId_ = 0xffffffffu;
  auto it = self->bound_.find(id);
  if (it != self->bound_.end()) {
    self->DestroyBound(it->second.get());
    self->bound_.erase(it);
  }
  if (mask != 0) self->Notify(mask);
}

void PwRegistry::OnNodeInfo(void* data, const pw_node_info* info) {
  auto* b = static_cast<Bound*>(data);
  PwRegistry* self = b->owner;
  if (info == nullptr || (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) == 0) return;
  {
    std::lock_guard<std::mutex> lock(self->mu_);
    auto it = self->nodes_.find(b->id);
    if (it == self->nodes_.end()) return;
    ApplyNodeProps(it->second, info->props);
  }
  self->Notify(kNodes);
}

void PwRegistry::OnNodeParam(void* data, int seq, uint32_t id, uint32_t index, uint32_t next, const spa_pod* param) {
  (void)seq;
  (void)index;
  (void)next;
  auto* b = static_cast<Bound*>(data);
  PwRegistry* self = b->owner;
  if (id != SPA_PARAM_Props) return;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(self->mu_);
    auto it = self->nodes_.find(b->id);
    if (it == self->nodes_.end()) return;
    const bool oldMute = it->second.mute;
    const float oldVol = it->second.maxVolume;
    const bool oldKnown = it->second.propsKnown;
    ParseProps(param, it->second);
    changed = oldMute != it->second.mute || oldVol != it->second.maxVolume || oldKnown != it->second.propsKnown;
  }
  if (changed) self->Notify(kProps);
}

void PwRegistry::OnDeviceInfo(void* data, const pw_device_info* info) {
  auto* b = static_cast<Bound*>(data);
  PwRegistry* self = b->owner;
  if (info == nullptr || (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) == 0) return;
  {
    std::lock_guard<std::mutex> lock(self->mu_);
    auto it = self->devices_.find(b->id);
    if (it == self->devices_.end()) return;
    ApplyDeviceProps(it->second, info->props);
  }
  self->Notify(kDevices);
}

void PwRegistry::OnDeviceParam(void* data, int seq, uint32_t id, uint32_t index, uint32_t next,
                               const spa_pod* param) {
  (void)seq;
  (void)index;
  (void)next;
  auto* b = static_cast<Bound*>(data);
  PwRegistry* self = b->owner;
  if (id != SPA_PARAM_Route) return;
  uint32_t direction = 0;
  PwRoute route;
  if (!ParseRoute(param, &direction, &route) || direction != SPA_DIRECTION_OUTPUT) return;
  {
    std::lock_guard<std::mutex> lock(self->mu_);
    auto it = self->devices_.find(b->id);
    if (it == self->devices_.end()) return;
    it->second.outputRoutes[route.device] = route;
  }
  self->Notify(kDevices);
}

int PwRegistry::OnMetadataProperty(void* data, uint32_t subject, const char* key, const char* type,
                                   const char* value) {
  (void)type;
  auto* b = static_cast<Bound*>(data);
  PwRegistry* self = b->owner;
  if (subject != PW_ID_CORE) return 0;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(self->mu_);
    if (key == nullptr) {
      changed = !self->defaultSource_.empty() || !self->defaultSink_.empty();
      self->defaultSource_.clear();
      self->defaultSink_.clear();
    } else if (std::strcmp(key, "default.audio.source") == 0) {
      std::string name = ParseMetadataName(value);
      changed = name != self->defaultSource_;
      self->defaultSource_ = std::move(name);
    } else if (std::strcmp(key, "default.audio.sink") == 0) {
      std::string name = ParseMetadataName(value);
      changed = name != self->defaultSink_;
      self->defaultSink_ = std::move(name);
    }
  }
  if (changed) self->Notify(kDefaults);
  return 0;
}

void PwRegistry::OnProxyDestroy(void* data) {
  // The proxy is going away underneath us (pw_core_disconnect or server-side
  // removal); drop our hooks and forget the pointer so it is never destroyed
  // twice. global_remove (if it still arrives) then only erases the entry.
  auto* b = static_cast<Bound*>(data);
  if (b->objectListenerAdded) {
    spa_hook_remove(&b->objectListener);
    b->objectListenerAdded = false;
  }
  if (b->proxyListenerAdded) {
    spa_hook_remove(&b->proxyListener);
    b->proxyListenerAdded = false;
  }
  b->proxy = nullptr;
}

RegistrySnapshot PwRegistry::Snapshot() const {
  RegistrySnapshot snap;
  std::lock_guard<std::mutex> lock(mu_);
  snap.nodes.reserve(nodes_.size());
  for (const auto& it : nodes_) snap.nodes.push_back(it.second);
  snap.devices = devices_;
  snap.links.reserve(links_.size());
  for (const auto& it : links_) snap.links.push_back(it.second);
  snap.defaultSourceName = defaultSource_;
  snap.defaultSinkName = defaultSink_;
  snap.metadataPresent = metadataId_ != 0xffffffffu;
  return snap;
}

const PwAudioNode* RegistrySnapshot::ById(uint32_t id) const {
  for (const auto& n : nodes)
    if (n.id == id) return &n;
  return nullptr;
}

const PwAudioNode* RegistrySnapshot::ByName(const std::string& name, bool sink) const {
  for (const auto& n : nodes)
    if (n.name == name && (sink ? n.isSink : n.isSource)) return &n;
  return nullptr;
}

namespace {

const PwAudioNode* PickDefault(const RegistrySnapshot& s, const std::string& configured, bool sink) {
  if (!configured.empty()) {
    if (const PwAudioNode* n = s.ByName(configured, sink)) return n;
  }
  const PwAudioNode* best = nullptr;
  for (const auto& n : s.nodes) {
    if (!(sink ? n.isSink : n.isSource)) continue;
    if (best == nullptr || n.prioritySession > best->prioritySession) best = &n;
  }
  return best;
}

}  // namespace

const PwAudioNode* RegistrySnapshot::DefaultSource() const { return PickDefault(*this, defaultSourceName, false); }

const PwAudioNode* RegistrySnapshot::DefaultSink() const { return PickDefault(*this, defaultSinkName, true); }

std::vector<const PwAudioNode*> RegistrySnapshot::Sources() const {
  std::vector<const PwAudioNode*> out;
  for (const auto& n : nodes)
    if (n.isSource) out.push_back(&n);
  return out;
}

std::vector<const PwAudioNode*> RegistrySnapshot::Sinks() const {
  std::vector<const PwAudioNode*> out;
  for (const auto& n : nodes)
    if (n.isSink) out.push_back(&n);
  return out;
}

uint32_t RegistrySnapshot::LinkedOutputNode(uint32_t streamNodeId) const {
  for (const auto& l : links)
    if (l.inputNode == streamNodeId) return l.outputNode;
  return 0;
}

const PwAudioDevice* RegistrySnapshot::DeviceOf(const PwAudioNode& node) const {
  auto it = devices.find(node.deviceId);
  return it != devices.end() ? &it->second : nullptr;
}

}  // namespace plaud_rec
