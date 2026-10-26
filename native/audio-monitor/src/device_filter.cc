// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "device_filter.h"

#include "proc_util.h"

namespace am {

std::string deviceDisplayName(const Props& props) {
  for (const char* key : {"node.description", "node.nick", "node.name"}) {
    std::string v = prop(props, key);
    if (!v.empty()) return v;
  }
  return std::string();
}

DeviceVerdict classifyDevice(const DeviceNode& node, DeviceDirection direction) {
  DeviceVerdict v;
  const std::string api = prop(node.props, "device.api");
  const std::string bus = prop(node.props, "device.bus");
  v.transport = !bus.empty() ? bus : (!api.empty() ? api : std::string("virtual"));
  const char* wantClass = direction == DeviceDirection::Capture ? "Audio/Source" : "Audio/Sink";
  if (node.mediaClass != wantClass) {
    v.reason = "media.class " + node.mediaClass;
    return v;
  }
  if (api != "alsa" && api != "bluez5") {
    v.reason = api.empty() ? "no hardware device.api" : "device.api " + api;
    return v;
  }
  if (prop(node.props, "node.virtual") == "true") {
    v.reason = "node.virtual";
    return v;
  }
  const std::string label = node.name + " " + node.description;
  if (icontains(label, "virtual") || icontains(label, "phone link")) {
    v.reason = "virtual device name";
    return v;
  }
  v.allowed = true;
  return v;
}

}  // namespace am
