// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "speaker_selector.h"

namespace am {

void SpeakerSelector::setCurrent(const std::string& uid) {
  current_ = uid;
  pending_.clear();
  consecutive_ = 0;
}

bool SpeakerSelector::evaluate(const std::vector<SpeakerLevel>& levels, SpeakerLevel& target, float& currentPeak) {
  currentPeak = 0.0f;
  const SpeakerLevel* best = nullptr;
  float bestPeak = 0.0f;
  for (const SpeakerLevel& d : levels) {
    if (d.uid == current_) {
      currentPeak = d.peak;
      continue;
    }
    if (best == nullptr || bestPeak < d.peak) {
      best = &d;
      bestPeak = d.peak;
    }
  }
  if (currentPeak > p_.currentActivePeak || best == nullptr || bestPeak <= p_.targetMinPeak) {
    consecutive_ = 0;
    pending_.clear();
    return false;
  }
  if (best->uid == pending_) {
    ++consecutive_;
  } else {
    pending_ = best->uid;
    consecutive_ = 1;
  }
  if (consecutive_ < p_.consecutiveToSwitch) return false;
  target = *best;
  current_ = best->uid;
  pending_.clear();
  consecutive_ = 0;
  return true;
}

}  // namespace am
