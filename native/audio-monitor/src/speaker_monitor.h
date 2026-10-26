// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Smart speaker monitor (startSpeakerMonitor/stopSpeakerMonitor), the
// Windows SpeakerMonitor on PipeWire: a passive peak meter on the monitor of
// every physical output, the SpeakerSelector every 500 ms on the engine's
// loop thread. Events: speakerAutoSelected, noSpeaker.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "device_filter.h"
#include "meter_stream.h"
#include "mic_monitor.h"
#include "pw_engine.h"
#include "speaker_selector.h"

namespace am {

class SpeakerMonitor : public GraphObserver {
 public:
  static constexpr int64_t kTickMs = 500;
  static constexpr int kDiagEvery = 60;
  static constexpr int64_t kRetryFailedMs = 10000;
  static constexpr int64_t kResumeGraceMs = 3000;

  SpeakerMonitor(PwEngine& engine, DeviceEventSink& sink);
  ~SpeakerMonitor() override;

  std::string start();
  void stop();
  bool running() const { return running_; }
  std::string currentUid();

  void onGraphChanged() override;
  void onConnectionLost() override;

 private:
  struct Device {
    std::string uid;
    std::string name;
  };

  void tick();
  void syncDevices(bool initial);
  void emit(const std::string& type, const std::string& uid, const std::string& name);

  PwEngine& engine_;
  DeviceEventSink& sink_;
  bool running_ = false;
  spa_source* timer_ = nullptr;
  SpeakerSelector selector_;
  int64_t evalCount_ = 0;
  bool syncPending_ = false;
  bool frozen_ = false;
  int64_t resumedAtMs_ = 0;  // last time the PipeWire connection came back
  bool noSpeakerReported_ = false;
  std::vector<Device> order_;  // allowed outputs with a live meter, default first
  std::map<std::string, std::unique_ptr<MeterStream>> meters_;
  std::map<std::string, int64_t> failedAt_;
  std::set<std::string> filteredLogged_;
  std::set<std::string> failureSeen_;  // meters seen failed once, judged next tick
  std::string defaultSink_;
};

}  // namespace am
