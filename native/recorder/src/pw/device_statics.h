// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Device-level statics of the Recorder class: device lists, the microphone
// list watcher, the default-output mute monitor and the system-audio
// "permission" probe. They share one lazily created PipeWire connection
// (DeviceHub) while a watcher or the mute monitor is active; one-shot
// queries otherwise use a short-lived connection.

#pragma once

#include <napi.h>

#include <cstdint>
#include <string>

#include "pw/pw_registry.h"

namespace plaud_rec {

Napi::Value CheckSystemAudioPermission(const Napi::CallbackInfo& info);
Napi::Value RequestSystemAudioPermission(const Napi::CallbackInfo& info);
Napi::Value ListMicrophoneDevices(const Napi::CallbackInfo& info);
Napi::Value ListMicrophoneDevicesDetailed(const Napi::CallbackInfo& info);
Napi::Value WatchMicrophoneDevices(const Napi::CallbackInfo& info);
Napi::Value StopWatchingMicrophoneDevices(const Napi::CallbackInfo& info);
Napi::Value ListSpeakerDevices(const Napi::CallbackInfo& info);
Napi::Value ListSpeakerDevicesDetailed(const Napi::CallbackInfo& info);
Napi::Value StartSpeakerMuteMonitor(const Napi::CallbackInfo& info);
Napi::Value StopSpeakerMuteMonitor(const Napi::CallbackInfo& info);
Napi::Value IsSpeakerMuted(const Napi::CallbackInfo& info);

// Current audio graph (from the running DeviceHub, or a one-shot connection).
bool TakeDeviceSnapshot(RegistrySnapshot* out, std::string* error);

}  // namespace plaud_rec
