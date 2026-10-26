// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Per-application meeting state machine (pure logic, clock injected).
//
// Mirrors the macOS addon's poll state machine (log texts included):
//   * native apps become ACTIVE as soon as they capture the microphone;
//   * browsers need WebRTC evidence that stays stable for 2.5 s
//     ("ACTIVE pending (start)" {stableMs:"2500"} / "ACTIVE pending (cancel)");
//   * when an active app stops capturing, INACTIVE is reported after a 3 s
//     grace period ("INACTIVE pending (start)" {delaySeconds:"3"},
//     "INACTIVE pending (keep)" on each 1 s tick, "CANCEL INACTIVE" when the
//     capture resumes in time);
//   * a complete turnover of an active app's capture streams is a new
//     session ("COUNTER RESET -> NEW SESSION") and is re-notified, subject to
//     the Windows addon's 2 s per-app notification throttle.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.h"

namespace am {

struct AppObservation {
  std::string appName;
  bool browser = false;
  bool udpEvidence = false;    // WebRTC-like UDP socket in the browser tree
  bool titleEvidence = false;  // a browser window shows a meeting service
  std::set<uint64_t> streams;  // object.serial of live microphone captures
  Props meta;                  // extra string meta for monitor events
};

struct MeetingLog {
  std::string level;
  std::string message;
  Props meta;
};

struct MeetingNotify {
  std::string appName;
  bool isActive = false;
  Props meta;
};

struct MeetingOutput {
  std::vector<MeetingLog> logs;
  std::vector<MeetingNotify> notifies;
};

struct MeetingTiming {
  int64_t browserStableMs = 2500;
  int64_t inactiveGraceMs = 3000;
  int64_t renotifyThrottleMs = 2000;
};

class MeetingStateMachine {
 public:
  explicit MeetingStateMachine(MeetingTiming timing = MeetingTiming()) : timing_(timing) {}

  // Apps absent from `observations` (or observed without streams) are
  // treated as not capturing. `tick` marks the periodic 1 s evaluation.
  void evaluate(int64_t nowMs, const std::vector<AppObservation>& observations, bool tick, MeetingOutput& out);

  // Earliest pending deadline (ms, same clock as evaluate), or -1.
  int64_t nextDeadline() const;
  bool isActive(const std::string& appName) const;
  const MeetingTiming& timing() const { return timing_; }

 private:
  struct AppState {
    bool active = false;
    bool pendingActive = false;
    int64_t pendingActiveSince = 0;
    bool pendingInactive = false;
    int64_t pendingInactiveSince = 0;
    bool sawUdp = false;
    std::set<uint64_t> streams;
    int64_t lastNotifyMs = 0;
    bool notifiedOnce = false;
    Props meta;
  };

  void activate(const std::string& app, AppState& st, const AppObservation& obs, int64_t nowMs, MeetingOutput& out);
  static Props notifyMeta(const std::string& app, bool active, const Props& extra);

  MeetingTiming timing_;
  std::map<std::string, AppState> apps_;
};

}  // namespace am
