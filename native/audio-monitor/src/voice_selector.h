// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Smart-microphone selection (pure logic, clock injected), the official
// VoiceEnergySelector evaluated every 200 ms over all physical microphones:
//   * voice present when any device level >= 0.005; after 50 consecutive
//     ticks (10 s) without voice -> weakVoice (once), the next voice ->
//     weakVoiceRecovered(best device);
//   * the loudest device is a switch candidate when its level >= 0.001, it is
//     not the current device and the last switch is >= 3 s ago;
//   * each tick records a "win" when best/current >= 1.3 + 0.7 * clamp(current/0.01, 0, 1)
//     (ratio 100 when current <= 1e-6); 5 wins inside a sliding 1.6 s window
//     switch to the candidate -> deviceAutoSelected.
// The ratio rule is the Windows addon's (macOS ships 0.91 + 1.0 * clamp);
// Linux follows the non-mac path per the project contract.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace am {

struct DeviceLevel {
  std::string uid;
  std::string name;
  float level = 0.0f;
};

struct VoiceSelectorParams {
  int64_t windowMs = 1600;
  int64_t minSwitchIntervalMs = 3000;
  int winsToSwitch = 5;
  float voiceThreshold = 0.005f;
  float minBestLevel = 0.001f;
  float normLevel = 0.01f;
  float ratioBase = 1.3f;
  float ratioSlope = 0.7f;
  float silentCurrent = 1e-6f;
  float silentRatio = 100.0f;
  int weakTicks = 50;
};

class VoiceSelector {
 public:
  enum class EventType { AutoSelected, WeakVoice, WeakVoiceRecovered, NoDevice };
  struct Event {
    EventType type;
    std::string uid;
    std::string name;
  };

  explicit VoiceSelector(VoiceSelectorParams params = VoiceSelectorParams()) : p_(params) {}

  // Forces the current device (initial pick, fallback); resets switching state.
  void setCurrent(const std::string& uid);
  const std::string& current() const { return current_; }
  bool weak() const { return weak_; }

  void evaluate(int64_t nowMs, const std::vector<DeviceLevel>& levels, std::vector<Event>& events);

 private:
  struct WindowEntry {
    int64_t atMs;
    bool win;
  };
  void clearCandidate();

  VoiceSelectorParams p_;
  std::string current_;
  std::string pending_;
  std::deque<WindowEntry> window_;
  int64_t lastSwitchMs_ = 0;
  bool hasSwitched_ = false;
  int weakCounter_ = 0;
  bool weak_ = false;
  bool noDeviceReported_ = false;
};

}  // namespace am
