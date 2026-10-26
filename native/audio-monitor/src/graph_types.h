// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Plain data describing the PipeWire graph, shared between the PipeWire
// thread (producer) and the meeting detector / device monitors (consumers).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace am {

// A Stream/Input/Audio node: an application capture stream.
struct CaptureStream {
  uint32_t nodeId = 0;
  uint64_t serial = 0;
  Props nodeProps;
  Props clientProps;
  bool running = false;    // node state "running"
  bool corked = false;     // pipewire-pulse "pulse.corked" (paused by the app)
  bool monitorOnly = false;  // stream.monitor (peak meters, not a real capture)
  bool micLinked = false;  // linked from an Audio/Source* (or Audio/Duplex) node
  std::string sourceName;  // node.name of the first linked microphone source
  std::string sourceDescription;
};

struct CaptureSnapshot {
  bool connected = false;
  uint64_t generation = 0;
  std::vector<CaptureStream> streams;
};

// An audio device node (Audio/Source* or Audio/Sink*) as seen by the device
// monitors.
struct DeviceNode {
  uint32_t id = 0;
  uint64_t serial = 0;
  std::string name;         // node.name  (the device UID shared with the Recorder addon)
  std::string description;  // node.description / node.nick / node.name
  std::string mediaClass;
  Props props;
};

}  // namespace am
