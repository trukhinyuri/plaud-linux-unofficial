// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Smart-speaker selection (pure logic), the Windows SpeakerMonitor rule
// evaluated every 500 ms over the peak meters of all physical outputs:
//   * stay while the current device plays (peak > 0.01) or no other device
//     plays (best other peak <= 0.02);
//   * otherwise the loudest other device becomes the pending switch target;
//     seeing the same target on 2 consecutive evaluations switches to it
//     (speakerAutoSelected).
#pragma once

#include <string>
#include <vector>

namespace am {

struct SpeakerLevel {
  std::string uid;
  std::string name;
  float peak = 0.0f;
};

struct SpeakerSelectorParams {
  float currentActivePeak = 0.01f;
  float targetMinPeak = 0.02f;
  int consecutiveToSwitch = 2;
};

class SpeakerSelector {
 public:
  explicit SpeakerSelector(SpeakerSelectorParams params = SpeakerSelectorParams()) : p_(params) {}

  void setCurrent(const std::string& uid);
  const std::string& current() const { return current_; }
  const std::string& pending() const { return pending_; }
  int consecutive() const { return consecutive_; }

  // Returns true when switching; `target` is the new device, `currentPeak`
  // the peak of the device being left.
  bool evaluate(const std::vector<SpeakerLevel>& levels, SpeakerLevel& target, float& currentPeak);

 private:
  SpeakerSelectorParams p_;
  std::string current_;
  std::string pending_;
  int consecutive_ = 0;
};

}  // namespace am
