// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "voice_selector.h"

#include <algorithm>

namespace am {

void VoiceSelector::clearCandidate() {
  window_.clear();
  pending_.clear();
}

void VoiceSelector::setCurrent(const std::string& uid) {
  current_ = uid;
  clearCandidate();
  hasSwitched_ = false;
}

void VoiceSelector::evaluate(int64_t nowMs, const std::vector<DeviceLevel>& levels, std::vector<Event>& events) {
  if (levels.empty()) {
    if (!noDeviceReported_) {
      noDeviceReported_ = true;
      events.push_back({EventType::NoDevice, std::string(), std::string()});
    }
    return;
  }
  noDeviceReported_ = false;

  while (!window_.empty() && window_.front().atMs < nowMs - p_.windowMs) window_.pop_front();

  const DeviceLevel* best = &levels.front();
  for (const DeviceLevel& d : levels) {
    if (d.level > best->level) best = &d;
  }

  bool voice = false;
  for (const DeviceLevel& d : levels) {
    if (d.level >= p_.voiceThreshold) {
      voice = true;
      break;
    }
  }
  if (voice) {
    if (weak_) {
      weak_ = false;
      events.push_back({EventType::WeakVoiceRecovered, best->uid, best->name});
    }
    weakCounter_ = 0;
  } else {
    ++weakCounter_;
    if (weakCounter_ == p_.weakTicks && !weak_) {
      weak_ = true;
      events.push_back({EventType::WeakVoice, std::string(), std::string()});
    }
  }

  if (best->level < p_.minBestLevel) return;
  if (best->uid == current_) {
    clearCandidate();
    return;
  }
  if (hasSwitched_ && nowMs - lastSwitchMs_ < p_.minSwitchIntervalMs) return;
  if (best->uid != pending_) {
    window_.clear();
    pending_ = best->uid;
  }

  float currentLevel = 0.0f;
  for (const DeviceLevel& d : levels) {
    if (d.uid == current_) {
      currentLevel = d.level;
      break;
    }
  }
  const float norm = std::min(1.0f, std::max(0.0f, currentLevel / p_.normLevel));
  const float ratio = currentLevel > p_.silentCurrent ? best->level / currentLevel : p_.silentRatio;
  window_.push_back({nowMs, ratio >= p_.ratioBase + p_.ratioSlope * norm});

  int wins = 0;
  for (const WindowEntry& e : window_) wins += e.win ? 1 : 0;
  if (wins < p_.winsToSwitch) return;

  clearCandidate();
  current_ = best->uid;
  lastSwitchMs_ = nowMs;
  hasSwitched_ = true;
  events.push_back({EventType::AutoSelected, best->uid, best->name});
}

}  // namespace am
