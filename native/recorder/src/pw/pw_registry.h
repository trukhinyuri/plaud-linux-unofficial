// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Live model of the PipeWire audio graph needed by the recorder: audio
// source/sink nodes (with full properties and, for sinks, mute/volume),
// their devices (form factor, bus, active routes), links, and the default
// source/sink from the "default" metadata.
//
// Mutated on the connection's loop thread; Snapshot() is thread-safe.

#pragma once

#include <pipewire/pipewire.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pw/pw_connection.h"

namespace plaud_rec {

struct PwRoute {
  int device = -1;  // card profile device index the route applies to
  std::string name;
  std::string description;
  std::string portType;  // ACP "port.type" (speaker, headphones, headset, hdmi, ...)
};

struct PwAudioDevice {
  uint32_t id = 0;
  std::string name;
  std::string description;
  std::string formFactor;  // device.form-factor
  std::string bus;         // device.bus
  std::string api;         // device.api
  std::map<int, PwRoute> outputRoutes;  // active output routes keyed by device index
};

struct PwAudioNode {
  uint32_t id = 0;
  uint64_t serial = 0;
  std::string name;         // node.name, the stable uid exposed to JS
  std::string description;  // node.description
  std::string nick;         // node.nick
  std::string mediaClass;
  uint32_t deviceId = 0xffffffffu;  // device.id
  std::string formFactor;           // device.form-factor set on the node itself (virtual/filter sinks)
  std::string bus;                  // device.bus set on the node itself
  int profileDevice = -1;           // card.profile.device
  int prioritySession = 0;
  bool isSource = false;
  bool isSink = false;
  bool propsKnown = false;
  bool mute = false;
  float maxVolume = 1.f;  // max of channelVolumes

  // Name shown to users, mirroring what desktop mixers display.
  std::string DisplayName() const {
    if (!description.empty()) return description;
    if (!nick.empty()) return nick;
    return name;
  }
};

struct PwLinkInfo {
  uint32_t id = 0;
  uint32_t outputNode = 0;
  uint32_t inputNode = 0;
};

struct RegistrySnapshot {
  std::vector<PwAudioNode> nodes;  // registry order (ascending global id)
  std::map<uint32_t, PwAudioDevice> devices;
  std::vector<PwLinkInfo> links;
  std::string defaultSourceName;  // from metadata, may name a missing node
  std::string defaultSinkName;
  bool metadataPresent = false;

  const PwAudioNode* ById(uint32_t id) const;
  const PwAudioNode* ByName(const std::string& name, bool sink) const;
  // Effective default: metadata default if present, else highest priority.session.
  const PwAudioNode* DefaultSource() const;
  const PwAudioNode* DefaultSink() const;
  std::vector<const PwAudioNode*> Sources() const;
  std::vector<const PwAudioNode*> Sinks() const;
  // Node feeding the input of `streamNodeId` (first link), or 0.
  uint32_t LinkedOutputNode(uint32_t streamNodeId) const;
  const PwAudioDevice* DeviceOf(const PwAudioNode& node) const;
};

class PwRegistry {
 public:
  enum Change : uint32_t {
    kNodes = 1u << 0,
    kDefaults = 1u << 1,
    kLinks = 1u << 2,
    kProps = 1u << 3,
    kDevices = 1u << 4,
  };
  using ChangeFn = std::function<void(uint32_t changeMask)>;

  explicit PwRegistry(PwConnection* connection);
  ~PwRegistry();
  PwRegistry(const PwRegistry&) = delete;
  PwRegistry& operator=(const PwRegistry&) = delete;

  // Both must be called with the connection's loop lock held.
  bool Start(ChangeFn onChange);
  void Stop();

  RegistrySnapshot Snapshot() const;

 private:
  struct Bound;
  static void OnGlobal(void* data, uint32_t id, uint32_t permissions, const char* type, uint32_t version,
                       const spa_dict* props);
  static void OnGlobalRemove(void* data, uint32_t id);
  static void OnNodeInfo(void* data, const pw_node_info* info);
  static void OnNodeParam(void* data, int seq, uint32_t id, uint32_t index, uint32_t next, const spa_pod* param);
  static void OnDeviceInfo(void* data, const pw_device_info* info);
  static void OnDeviceParam(void* data, int seq, uint32_t id, uint32_t index, uint32_t next, const spa_pod* param);
  static int OnMetadataProperty(void* data, uint32_t subject, const char* key, const char* type, const char* value);
  static void OnProxyDestroy(void* data);

  void Notify(uint32_t mask);
  void DestroyBound(Bound* bound);

  PwConnection* conn_;
  pw_registry* registry_ = nullptr;
  spa_hook registryListener_{};
  ChangeFn onChange_;
  std::map<uint32_t, std::unique_ptr<Bound>> bound_;  // nodes, devices, metadata
  uint32_t metadataId_ = 0xffffffffu;

  mutable std::mutex mu_;
  std::map<uint32_t, PwAudioNode> nodes_;
  std::map<uint32_t, PwAudioDevice> devices_;
  std::map<uint32_t, PwLinkInfo> links_;
  std::string defaultSource_;
  std::string defaultSink_;
};

}  // namespace plaud_rec
