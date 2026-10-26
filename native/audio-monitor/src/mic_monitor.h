// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Smart microphone monitor (startMicMonitor/stopMicMonitor): meters every
// physical microphone and runs the VoiceSelector every 200 ms on the engine's
// loop thread. Events: deviceAutoSelected, deviceFallbackRequired,
// weakVoice, weakVoiceRecovered, noDevice.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "device_filter.h"
#include "meter_stream.h"
#include "pw_engine.h"
#include "voice_selector.h"

namespace am {

class DeviceEventSink {
 public:
  virtual ~DeviceEventSink() = default;
  virtual void deviceEvent(const std::string& eventType, const std::string& deviceId,
                           const std::string& deviceName) = 0;
};

class MicMonitor : public GraphObserver {
 public:
  static constexpr int64_t kTickMs = 200;
  static constexpr int kSyncEveryTicks = 50;
  static constexpr int64_t kRetryFailedMs = 10000;
  static constexpr int64_t kResumeGraceMs = 3000;

  MicMonitor(PwEngine& engine, DeviceEventSink& sink);
  ~MicMonitor() override;

  // JS thread. Returns the initially selected device UID ("" if none).
  std::string start();
  void stop();
  bool running() const { return running_; }
  // JS thread: current selection (takes the engine lock).
  std::string currentUid();

  void onGraphChanged() override;
  void onConnectionLost() override;

 private:
  void tick();
  void syncDevices();
  std::vector<DeviceLevel> levels() const;
  void emit(const std::string& type, const std::string& uid, const std::string& name);

  PwEngine& engine_;
  DeviceEventSink& sink_;
  bool running_ = false;
  spa_source* timer_ = nullptr;
  VoiceSelector selector_;
  int64_t tickCount_ = 0;
  bool syncPending_ = false;
  bool frozen_ = false;
  int64_t resumedAtMs_ = 0;  // last time the PipeWire connection came back
  std::vector<std::string> order_;  // allowed device UIDs, default first
  std::map<std::string, std::unique_ptr<MeterStream>> meters_;
  std::map<std::string, int64_t> failedAt_;
  std::set<std::string> filteredLogged_;
  std::set<std::string> failureSeen_;  // meters seen failed once, judged next tick
};

}  // namespace am
