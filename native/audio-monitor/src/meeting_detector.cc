// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "meeting_detector.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>

namespace am {

MeetingDetector::MeetingDetector(MonitorSink& sink, int selfPid) : sink_(sink), resolver_(selfPid) {}

MeetingDetector::~MeetingDetector() { stop(); }

void MeetingDetector::start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (running_) return;
  running_ = true;
  stopping_ = false;
  thread_ = std::thread(&MeetingDetector::run, this);
}

void MeetingDetector::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lk(mu_);
  running_ = false;
}

void MeetingDetector::post(CaptureSnapshot snapshot) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    latest_ = std::move(snapshot);
    if (!dirty_) {
      dirty_ = true;
      dirtyAtMs_ = steadyNowMs();
    }
  }
  cv_.notify_all();
}

void MeetingDetector::run() {
  std::unique_lock<std::mutex> lk(mu_);
  int64_t nextTick = steadyNowMs() + kTickMs;
  while (!stopping_) {
    const int64_t now = steadyNowMs();
    int64_t wake = nextTick;
    if (dirty_) wake = std::min(wake, dirtyAtMs_ + kDebounceMs);
    const int64_t deadline = machine_.nextDeadline();
    if (deadline >= 0) wake = std::min(wake, deadline);
    if (wake > now) {
      cv_.wait_for(lk, std::chrono::milliseconds(wake - now));
      continue;
    }
    const bool tick = now >= nextTick;
    if (tick) nextTick = now + kTickMs;
    dirty_ = false;
    CaptureSnapshot snapshot = latest_;
    lk.unlock();
    evaluate(snapshot, now, tick);
    lk.lock();
  }
}

MeetingDetector::EvidenceResult MeetingDetector::evidenceFor(int rootPid, int pid, int64_t nowMs, ProcessTable& table,
                                                             bool& tableFresh, std::vector<WindowInfo>& windows,
                                                             bool& windowsFetched) {
  const int key = rootPid > 0 ? rootPid : pid;
  auto cached = evidenceCache_.find(key);
  if (cached != evidenceCache_.end() && nowMs - cached->second.atMs < kEvidenceCacheMs) return cached->second;

  if (!tableFresh) {
    table.refresh();
    tableFresh = true;
  }
  std::vector<int> tree = table.subtree(key);
  if (tree.empty() && pid > 0) tree.push_back(pid);

  EvidenceResult r;
  r.atMs = nowMs;
  r.udp = scanner_.scan(tree, &r.stats);
  if (!r.udp) {
    if (!windowProviderChecked_) {
      windowProviderChecked_ = true;
      windowProvider_ = makeDefaultWindowProvider();
      sink_.log("info",
                std::string("Window title provider: ") +
                    (windowProvider_ ? windowProvider_->name() : std::string("none (WebRTC UDP evidence only)")),
                {});
    }
    if (windowProvider_ && !windowsFetched) {
      windowsFetched = true;
      windows.clear();
      if (!windowProvider_->list(windows)) windows.clear();
    }
    std::unordered_set<int> treeSet(tree.begin(), tree.end());
    for (const WindowInfo& w : windows) {
      if (w.pid <= 0 || !treeSet.count(w.pid)) continue;
      std::string service = meetingServiceFromTitle(w.title);
      if (!service.empty()) {
        r.title = true;
        r.titleService = service;
        break;
      }
    }
  }
  evidenceCache_[key] = r;
  return r;
}

void MeetingDetector::evaluate(const CaptureSnapshot& snapshot, int64_t nowMs, bool tick) {
  struct Group {
    AppObservation obs;
    std::vector<std::pair<int, int>> pidRoots;  // (pid, root pid)
    std::vector<uint64_t> serials;
  };
  std::map<std::string, Group> groups;
  std::set<uint64_t> seen;
  std::set<uint64_t> nowRunning;
  ProcessTable table;
  bool tableFresh = false;

  for (const CaptureStream& s : snapshot.streams) {
    seen.insert(s.serial);
    if (!s.micLinked || s.monitorOnly) continue;
    AppIdentity fresh;
    const AppIdentity* idp = nullptr;
    auto it = identityCache_.find(s.serial);
    if (it != identityCache_.end()) {
      idp = &it->second;
    } else {
      if (!tableFresh) {
        table.refresh();
        tableFresh = true;
      }
      fresh = resolver_.resolve(s.nodeProps, s.clientProps, table);
      // The client's info can arrive after its node; only a resolution that
      // saw the client (pid source) is final.
      if (!s.clientProps.empty()) {
        idp = &identityCache_.emplace(s.serial, fresh).first->second;
      } else {
        idp = &fresh;
      }
    }
    const AppIdentity& id = *idp;
    if (id.isSelf || id.isSystem) continue;
    if (!s.running || s.corked) continue;
    nowRunning.insert(s.serial);
    if (!runningSerials_.count(s.serial)) {
      sink_.log("info",
                "Audio PID " + std::to_string(id.pid) + " matched capture stream: " + id.appName + " (via PID " +
                    std::to_string(id.rootPid > 0 ? id.rootPid : id.pid) + ")",
                {{"processName", id.processName},
                 {"matchedBy", id.matchedBy.empty() ? "raw" : id.matchedBy},
                 {"deviceId", s.sourceName},
                 {"nodeId", std::to_string(s.nodeId)}});
    }
    Group& g = groups[id.appName];
    if (g.obs.appName.empty()) {
      g.obs.appName = id.appName;
      g.obs.browser = id.isBrowser();
      g.obs.meta["processName"] = id.processName;
      g.obs.meta["pid"] = std::to_string(id.pid);
      g.obs.meta["deviceId"] = s.sourceName;
      g.obs.meta["deviceName"] = s.sourceDescription;
      g.obs.meta["audioDirection"] = "input";
      if (!id.flatpakId.empty()) g.obs.meta["flatpakAppId"] = id.flatpakId;
      if (!id.snapName.empty()) g.obs.meta["snapName"] = id.snapName;
      if (id.entry != nullptr && id.entry->isBrowser()) g.obs.meta["browserEngine"] = engineName(id.entry->engine);
    }
    g.obs.streams.insert(s.serial);
    g.pidRoots.emplace_back(id.pid, id.rootPid);
    g.serials.push_back(s.serial);
  }

  // Streams that stopped capturing since the previous evaluation.
  for (uint64_t serial : runningSerials_) {
    if (nowRunning.count(serial)) continue;
    auto it = identityCache_.find(serial);
    const int pid = it != identityCache_.end() ? it->second.pid : 0;
    sink_.log("info", "Audio PID " + std::to_string(pid) + " lost capture stream (no longer in meeting)", {});
  }
  runningSerials_ = nowRunning;
  for (auto it = identityCache_.begin(); it != identityCache_.end();) {
    it = seen.count(it->first) ? std::next(it) : identityCache_.erase(it);
  }
  for (auto it = ignoredSerials_.begin(); it != ignoredSerials_.end();) {
    it = nowRunning.count(*it) ? std::next(it) : ignoredSerials_.erase(it);
  }

  std::vector<WindowInfo> windows;
  bool windowsFetched = false;
  std::set<int> liveRoots;
  std::vector<AppObservation> observations;
  for (auto& kv : groups) {
    Group& g = kv.second;
    if (g.obs.browser) {
      std::set<int> roots;
      for (const auto& pr : g.pidRoots) {
        const int root = pr.second > 0 ? pr.second : pr.first;
        if (!roots.insert(root).second) continue;
        liveRoots.insert(root);
        EvidenceResult e = evidenceFor(pr.second, pr.first, nowMs, table, tableFresh, windows, windowsFetched);
        if (e.udp && !g.obs.udpEvidence) {
          g.obs.udpEvidence = true;
          g.obs.meta["evidence"] = "webrtc-udp";
          g.obs.meta["udpSocket"] = e.stats.sampleAccepted;
        }
        if (e.title && !g.obs.titleEvidence) {
          g.obs.titleEvidence = true;
          if (!g.obs.udpEvidence) g.obs.meta["evidence"] = "window-title";
          g.obs.meta["meetingService"] = e.titleService;
        }
        if (!e.udp && !e.title) {
          for (size_t i = 0; i < g.pidRoots.size(); ++i) {
            if (g.pidRoots[i] != pr || ignoredSerials_.count(g.serials[i])) continue;
            ignoredSerials_.insert(g.serials[i]);
            sink_.log("debug",
                      "Audio PID " + std::to_string(pr.first) +
                          " ignored (browser without WebRTC evidence): " + g.obs.appName,
                      {{"udpSockets", std::to_string(e.stats.sockets)},
                       {"rejConnected", std::to_string(e.stats.rejConnected)},
                       {"rejLoopback", std::to_string(e.stats.rejLoopback)},
                       {"rejMulticast", std::to_string(e.stats.rejMulticast)},
                       {"rejReservedPort", std::to_string(e.stats.rejReservedPort)},
                       {"unreadablePids", std::to_string(e.stats.unreadablePids)}});
          }
        }
      }
    }
    observations.push_back(g.obs);
  }
  for (auto it = evidenceCache_.begin(); it != evidenceCache_.end();) {
    it = liveRoots.count(it->first) ? std::next(it) : evidenceCache_.erase(it);
  }

  MeetingOutput out;
  machine_.evaluate(nowMs, observations, tick, out);
  for (const MeetingLog& l : out.logs) sink_.log(l.level, l.message, l.meta);
  for (const MeetingNotify& n : out.notifies) sink_.monitor(n.appName, n.isActive, n.meta);
}

}  // namespace am
