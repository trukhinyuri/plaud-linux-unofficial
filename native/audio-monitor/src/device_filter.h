// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Which PipeWire device nodes the smart microphone/speaker monitors consider:
// physical hardware only, like the official addons (macOS: transport type
// BuiltIn/USB/Thunderbolt/Bluetooth, no Virtual/Aggregate/AirPlay; Windows:
// active endpoints whose name does not contain "virtual" or "phone link").
//
// Linux rule: media.class exactly "Audio/Source" (microphones) or
// "Audio/Sink" (outputs), device.api "alsa" or "bluez5", not node.virtual,
// and the name/description does not contain "virtual" or "phone link".
#pragma once

#include <string>

#include "graph_types.h"

namespace am {

enum class DeviceDirection { Capture, Render };

struct DeviceVerdict {
  bool allowed = false;
  std::string transport;  // device.bus (usb, pci, bluetooth, platform...) or device.api
  std::string reason;     // why it was filtered
};

// Physical devices only (the mac/win addons skip virtual devices too):
// media.class exactly Audio/Source (capture) or Audio/Sink (render),
// device.api alsa or bluez5, not node.virtual, and no "virtual" or
// "phone link" in the node name or description.
DeviceVerdict classifyDevice(const DeviceNode& node, DeviceDirection direction);

// node.description, else node.nick, else node.name.
std::string deviceDisplayName(const Props& props);

}  // namespace am
