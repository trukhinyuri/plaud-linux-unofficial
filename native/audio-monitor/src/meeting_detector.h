// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Meeting detector worker: turns PipeWire capture snapshots into
// per-application observations (identity, browser evidence) and runs the
// MeetingStateMachine on its own thread (1 s tick, 100 ms debounce of graph
// changes), so /proc scans and compositor IPC never stall the PipeWire loop.
#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "app_identity.h"
#include "graph_types.h"
#include "meeting_state.h"
#include "webrtc_evidence.h"
#include "window_titles.h"

namespace am {

class MonitorSink {
 public:
  virtual ~MonitorSink() = default;
  virtual void log(const std::string& level, const std::string& message, const Props& meta) = 0;
  virtual void monitor(const std::string& appName, bool isActive, const Props& meta) = 0;
};

class MeetingDetector {
 public:
  static constexpr int64_t kTickMs = 1000;
  static constexpr int64_t kDebounceMs = 100;
  static constexpr int64_t kEvidenceCacheMs = 500;

  MeetingDetector(MonitorSink& sink, int selfPid);
  ~MeetingDetector();

  void start();
  void stop();
  // Thread-safe: latest graph view from the PipeWire thread.
  void post(CaptureSnapshot snapshot);

 private:
  struct EvidenceResult {
    int64_t atMs = 0;
    bool udp = false;
    bool title = false;
    std::string titleService;
    UdpScanStats stats;
  };

  void run();
  void evaluate(const CaptureSnapshot& snapshot, int64_t nowMs, bool tick);
  EvidenceResult evidenceFor(int rootPid, int pid, int64_t nowMs, ProcessTable& table, bool& tableFresh,
                             std::vector<WindowInfo>& windows, bool& windowsFetched);

  MonitorSink& sink_;
  IdentityResolver resolver_;
  MeetingStateMachine machine_;
  UdpEvidenceScanner scanner_;
  std::unique_ptr<WindowTitleProvider> windowProvider_;
  bool windowProviderChecked_ = false;

  std::map<uint64_t, AppIdentity> identityCache_;  // by stream serial
  std::set<uint64_t> runningSerials_;               // streams reported as matched
  std::set<uint64_t> ignoredSerials_;               // browser streams reported as ignored
  std::map<int, EvidenceResult> evidenceCache_;     // by browser root pid

  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool running_ = false;
  bool stopping_ = false;
  bool dirty_ = false;
  int64_t dirtyAtMs_ = 0;
  CaptureSnapshot latest_;
};

}  // namespace am
