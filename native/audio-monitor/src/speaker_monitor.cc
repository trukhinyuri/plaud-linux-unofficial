// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "speaker_monitor.h"

#include <algorithm>
#include <cstdio>

namespace am {

namespace {

std::string fmtFloat(const char* format, double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), format, v);
  return buf;
}

}  // namespace

SpeakerMonitor::SpeakerMonitor(PwEngine& engine, DeviceEventSink& sink) : engine_(engine), sink_(sink) {}

SpeakerMonitor::~SpeakerMonitor() { stop(); }

std::string SpeakerMonitor::start() {
  if (running_) return currentUid();
  engine_.addUser();
  PwLock lock(engine_);
  engine_.addObserver(this);
  const bool synced = engine_.waitSynced(3000);
  selector_ = SpeakerSelector();
  evalCount_ = 0;
  syncPending_ = false;
  frozen_ = false;
  resumedAtMs_ = 0;
  noSpeakerReported_ = false;
  failedAt_.clear();
  filteredLogged_.clear();
  if (!synced) engine_.log("warn", "[SpeakerMonitor] PipeWire not available at start, waiting for devices");
  syncDevices(true);
  const std::string initial = selector_.current();
  timer_ = engine_.addTimer([this] { tick(); });
  engine_.armTimer(timer_, kTickMs, kTickMs);
  running_ = true;
  engine_.log("info", "[SpeakerMonitor] Started", {{"devices", std::to_string(order_.size())}, {"initialUID", initial}});
  return initial;
}

void SpeakerMonitor::stop() {
  if (!running_) return;
  {
    PwLock lock(engine_);
    engine_.removeObserver(this);
    engine_.destroyTimer(timer_);
    timer_ = nullptr;
    meters_.clear();
    failureSeen_.clear();
    order_.clear();
    running_ = false;
    engine_.log("info", "[SpeakerMonitor] Stopped");
  }
  engine_.removeUser();
}

std::string SpeakerMonitor::currentUid() {
  PwLock lock(engine_);
  return selector_.current();
}

void SpeakerMonitor::onGraphChanged() { syncPending_ = true; }

void SpeakerMonitor::onConnectionLost() {
  if (!meters_.empty()) engine_.log("info", "[SpeakerMonitor] PipeWire connection lost, metering paused");
  meters_.clear();
  failureSeen_.clear();
  order_.clear();
  frozen_ = true;
}

void SpeakerMonitor::emit(const std::string& type, const std::string& uid, const std::string& name) {
  sink_.deviceEvent(type, uid, name);
}

void SpeakerMonitor::syncDevices(bool initial) {
  const int64_t now = steadyNowMs();
  std::vector<DeviceNode> all = engine_.devices("Audio/Sink");
  std::vector<DeviceNode> allowed;
  for (DeviceNode& d : all) {
    DeviceVerdict v = classifyDevice(d, DeviceDirection::Render);
    if (!v.allowed) {
      if (filteredLogged_.insert(d.name).second) {
        engine_.log("debug", "[SpeakerMonitor] Filtered output device: " + d.description + " transport=" + v.transport,
                    {{"uid", d.name}, {"reason", v.reason}});
      }
      continue;
    }
    allowed.push_back(std::move(d));
  }
  defaultSink_ = engine_.defaultSinkName();
  std::stable_partition(allowed.begin(), allowed.end(), [&](const DeviceNode& d) { return d.name == defaultSink_; });

  auto isAllowed = [&](const std::string& uid) {
    return std::any_of(allowed.begin(), allowed.end(), [&](const DeviceNode& d) { return d.name == uid; });
  };
  bool changed = false;
  for (const auto& kv : meters_) changed = changed || !isAllowed(kv.first);
  for (const DeviceNode& d : allowed) changed = changed || !meters_.count(d.name);
  if (changed && !initial) engine_.log("info", "[SpeakerMonitor] Device change detected, rescanning");

  for (auto it = meters_.begin(); it != meters_.end();) {
    if (isAllowed(it->first)) {
      ++it;
      continue;
    }
    engine_.log("info", "[SpeakerMonitor] Device removed: " + it->second->name(), {{"uid", it->first}});
    failureSeen_.erase(it->first);
    it = meters_.erase(it);
  }
  order_.clear();
  for (const DeviceNode& d : allowed) {
    if (!meters_.count(d.name)) {
      auto failed = failedAt_.find(d.name);
      if (failed != failedAt_.end() && now - failed->second < kRetryFailedMs) continue;
      std::unique_ptr<MeterStream> meter(new MeterStream(engine_, MeterStream::Kind::SinkMonitor, d, [this](MeterStream& s) {
        engine_.log("debug", "[SpeakerMonitor] Metering started: " + s.name() + " uid=" + s.uid() + " fmt=" + s.formatDescription());
      }));
      std::string error;
      if (!meter->connect(error)) {
        engine_.log("error", "[SpeakerMonitor] Metering FAILED: " + d.description, {{"uid", d.name}, {"error", error}});
        failedAt_[d.name] = now;
        continue;
      }
      failedAt_.erase(d.name);
      engine_.log("info", "[SpeakerMonitor] Device added: " + d.description + " uid=" + d.name);
      meters_[d.name] = std::move(meter);
    }
    order_.push_back({d.name, d.description});
  }

  const std::string cur = selector_.current();
  if (initial) {
    if (order_.empty()) {
      engine_.log("warn", "[SpeakerMonitor] No render device available");
      noSpeakerReported_ = true;
      emit("noSpeaker", "", "");
    } else {
      const Device& first = order_.front();
      selector_.setCurrent(first.uid);
      if (first.uid == defaultSink_) {
        engine_.log("info", "[SpeakerMonitor] Initial device by system default: " + first.name + " (peak=0.0000)",
                    {{"uid", first.uid}});
      } else {
        engine_.log("info", "[SpeakerMonitor] Initial device by fallback: " + first.name, {{"uid", first.uid}});
      }
    }
    return;
  }
  const bool present = std::any_of(order_.begin(), order_.end(), [&](const Device& d) { return d.uid == cur; });
  if (present) return;
  // After a (re)connection the session manager re-announces outputs one by
  // one: wait for the selected output instead of falling back, and do not
  // report "no output" yet. tick() re-syncs when the grace period ends.
  if (resumedAtMs_ > 0 && (!cur.empty() || order_.empty())) return;
  if (order_.empty()) {
    if (!noSpeakerReported_) {
      selector_.setCurrent("");
      noSpeakerReported_ = true;
      engine_.log("warn", "[SpeakerMonitor] No render device available");
      emit("noSpeaker", "", "");
    }
    return;
  }
  const Device& fb = order_.front();
  selector_.setCurrent(fb.uid);
  noSpeakerReported_ = false;
  engine_.log("info",
              (cur.empty() ? "[SpeakerMonitor] Device available, selecting: " : "[SpeakerMonitor] Device lost, fallback: ") +
                  fb.name,
              {{"uid", fb.uid}});
  emit("speakerAutoSelected", fb.uid, fb.name);
}

void SpeakerMonitor::tick() {
  if (!engine_.connected() || !engine_.synced()) {
    frozen_ = true;
    return;
  }
  const int64_t now = steadyNowMs();
  if (frozen_) {
    frozen_ = false;
    syncPending_ = true;
    resumedAtMs_ = now;
  } else if (resumedAtMs_ > 0 && now - resumedAtMs_ >= kResumeGraceMs) {
    resumedAtMs_ = 0;
    syncPending_ = true;
  }
  for (auto it = meters_.begin(); it != meters_.end();) {
    const std::string uid = it->first;
    if (!it->second->failed()) {
      failureSeen_.erase(uid);
      ++it;
      continue;
    }
    // Unplugged outputs also fail their meter; judge on the next tick, once the
    // registry has reported a removal.
    const std::vector<DeviceNode> present = engine_.devices("Audio/Sink");
    const bool exists = std::any_of(present.begin(), present.end(), [&](const DeviceNode& d) { return d.name == uid; });
    if (exists && failureSeen_.insert(uid).second) {
      ++it;
      continue;
    }
    failureSeen_.erase(uid);
    if (exists) {
      engine_.log("warn", "[SpeakerMonitor] Device died, metering stopped: " + it->second->name(),
                  {{"uid", uid}, {"reason", it->second->failure()}});
      failedAt_[uid] = now;
    } else {
      engine_.log("info", "[SpeakerMonitor] Device removed: " + it->second->name(), {{"uid", uid}});
    }
    it = meters_.erase(it);
    syncPending_ = true;
  }
  if (syncPending_) {
    syncPending_ = false;
    syncDevices(false);
  }

  std::vector<SpeakerLevel> levels;
  levels.reserve(order_.size());
  for (const Device& d : order_) {
    auto it = meters_.find(d.uid);
    if (it != meters_.end()) levels.push_back({d.uid, d.name, it->second->takePeak()});
  }

  if (++evalCount_ % kDiagEvery == 0) {
    std::string line = "[SpeakerMonitor][diag-eval] currentUID=" + selector_.current() +
                       " pendingSwitchUID=" + selector_.pending() +
                       " consecutive=" + std::to_string(selector_.consecutive()) + " | devices:";
    for (const SpeakerLevel& l : levels) {
      auto it = meters_.find(l.uid);
      const bool streaming = it != meters_.end() && it->second->streaming();
      line += " {name='" + l.name + "' state=" + (streaming ? "streaming" : "idle") +
              " peak=" + fmtFloat("%.4f", l.peak) + "}";
    }
    engine_.log("debug", line);
  }

  SpeakerLevel target;
  float currentPeak = 0.0f;
  if (selector_.evaluate(levels, target, currentPeak)) {
    engine_.log("info", "[SpeakerMonitor] Switch \xE2\x86\x92 " + target.name + " uid=" + target.uid +
                            ", target peak=" + fmtFloat("%f", target.peak) + " (current peak=" +
                            fmtFloat("%f", currentPeak) + ")");
    emit("speakerAutoSelected", target.uid, target.name);
  }
}

}  // namespace am
