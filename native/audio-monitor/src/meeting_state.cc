// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "meeting_state.h"

#include <algorithm>

namespace am {

namespace {

bool disjoint(const std::set<uint64_t>& a, const std::set<uint64_t>& b) {
  for (uint64_t v : a) {
    if (b.count(v)) return false;
  }
  return true;
}

}  // namespace

Props MeetingStateMachine::notifyMeta(const std::string& app, bool active, const Props& extra) {
  Props meta = extra;
  meta["appName"] = app;
  meta["isActive"] = active ? "1" : "0";
  return meta;
}

void MeetingStateMachine::activate(const std::string& app, AppState& st, const AppObservation& obs, int64_t nowMs,
                                   MeetingOutput& out) {
  st.active = true;
  st.pendingActive = false;
  st.pendingInactive = false;
  st.streams = obs.streams;
  st.lastNotifyMs = nowMs;
  st.notifiedOnce = true;
  st.meta = obs.meta;
  out.logs.push_back({"info", "ACTIVE " + app, {}});
  out.notifies.push_back({app, true, notifyMeta(app, true, obs.meta)});
}

void MeetingStateMachine::evaluate(int64_t nowMs, const std::vector<AppObservation>& observations, bool tick,
                                   MeetingOutput& out) {
  std::set<std::string> present;
  for (const AppObservation& obs : observations) {
    if (obs.streams.empty()) continue;
    auto existing = apps_.find(obs.appName);
    const bool known = existing != apps_.end();
    const bool wasActive = known && existing->second.active;
    const bool sawUdp = known && existing->second.sawUdp;
    bool isPresent = true;
    if (obs.browser) {
      // A browser session activated without UDP evidence (title only, e.g.
      // TURN over TCP) lasts while the capture lasts; a session that had UDP
      // media evidence ends when that evidence disappears.
      isPresent = obs.udpEvidence || obs.titleEvidence || (wasActive && !sawUdp);
    }
    if (!isPresent) continue;
    present.insert(obs.appName);
    AppState& st = apps_[obs.appName];
    if (obs.udpEvidence) st.sawUdp = true;
    if (st.pendingInactive) {
      st.pendingInactive = false;
      out.logs.push_back({"info", "CANCEL INACTIVE " + obs.appName, {}});
    }
    if (st.active) {
      if (!st.streams.empty() && disjoint(st.streams, obs.streams)) {
        out.logs.push_back({"info", "COUNTER RESET -> NEW SESSION " + obs.appName, {}});
        if (nowMs - st.lastNotifyMs >= timing_.renotifyThrottleMs) {
          st.lastNotifyMs = nowMs;
          out.logs.push_back({"info", "ACTIVE " + obs.appName, {}});
          out.notifies.push_back({obs.appName, true, notifyMeta(obs.appName, true, obs.meta)});
        }
      }
      st.streams = obs.streams;
      st.meta = obs.meta;
      continue;
    }
    if (obs.browser) {
      if (!st.pendingActive) {
        st.pendingActive = true;
        st.pendingActiveSince = nowMs;
        out.logs.push_back({"info", "ACTIVE pending (start) " + obs.appName,
                            {{"stableMs", std::to_string(timing_.browserStableMs)}}});
      }
      if (nowMs - st.pendingActiveSince >= timing_.browserStableMs) activate(obs.appName, st, obs, nowMs, out);
      continue;
    }
    activate(obs.appName, st, obs, nowMs, out);
  }

  for (auto it = apps_.begin(); it != apps_.end();) {
    const std::string& app = it->first;
    AppState& st = it->second;
    if (present.count(app)) {
      ++it;
      continue;
    }
    if (st.pendingActive) {
      st.pendingActive = false;
      out.logs.push_back({"info", "ACTIVE pending (cancel) " + app, {}});
    }
    if (!st.active) {
      it = apps_.erase(it);
      continue;
    }
    if (!st.pendingInactive) {
      st.pendingInactive = true;
      st.pendingInactiveSince = nowMs;
      out.logs.push_back(
          {"info", "INACTIVE pending (start) " + app, {{"delaySeconds", std::to_string(timing_.inactiveGraceMs / 1000)}}});
      ++it;
      continue;
    }
    if (nowMs - st.pendingInactiveSince >= timing_.inactiveGraceMs) {
      out.logs.push_back({"info", "INACTIVE " + app, {}});
      out.notifies.push_back({app, false, notifyMeta(app, false, st.meta)});
      it = apps_.erase(it);
      continue;
    }
    if (tick) out.logs.push_back({"info", "INACTIVE pending (keep) " + app, {}});
    ++it;
  }
}

int64_t MeetingStateMachine::nextDeadline() const {
  int64_t deadline = -1;
  for (const auto& kv : apps_) {
    const AppState& st = kv.second;
    if (st.pendingActive) {
      int64_t d = st.pendingActiveSince + timing_.browserStableMs;
      deadline = deadline < 0 ? d : std::min(deadline, d);
    }
    if (st.pendingInactive) {
      int64_t d = st.pendingInactiveSince + timing_.inactiveGraceMs;
      deadline = deadline < 0 ? d : std::min(deadline, d);
    }
  }
  return deadline;
}

bool MeetingStateMachine::isActive(const std::string& appName) const {
  auto it = apps_.find(appName);
  return it != apps_.end() && it->second.active;
}

}  // namespace am
