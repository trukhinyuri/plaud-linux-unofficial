// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "mic_monitor.h"

#include <algorithm>

namespace am {

MicMonitor::MicMonitor(PwEngine& engine, DeviceEventSink& sink) : engine_(engine), sink_(sink) {}

MicMonitor::~MicMonitor() { stop(); }

std::string MicMonitor::start() {
  if (running_) return currentUid();
  engine_.addUser();
  PwLock lock(engine_);
  engine_.addObserver(this);
  const bool synced = engine_.waitSynced(3000);
  selector_ = VoiceSelector();
  tickCount_ = 0;
  syncPending_ = false;
  frozen_ = false;
  resumedAtMs_ = 0;
  failedAt_.clear();
  filteredLogged_.clear();
  syncDevices();
  std::string initial;
  if (!order_.empty()) {
    initial = order_.front();
    selector_.setCurrent(initial);
    engine_.log("info", "Initial device: " + meters_[initial]->name() + " uid=" + initial);
  } else {
    engine_.log("warn", synced ? "[MicMonitor] No physical input device available at start"
                               : "[MicMonitor] PipeWire not available at start, waiting for devices");
  }
  timer_ = engine_.addTimer([this] { tick(); });
  engine_.armTimer(timer_, kTickMs, kTickMs);
  running_ = true;
  engine_.log("info", "[MicMonitor] Started", {{"devices", std::to_string(order_.size())}, {"initialUID", initial}});
  return initial;
}

void MicMonitor::stop() {
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
    engine_.log("info", "[MicMonitor] Stopped");
  }
  engine_.removeUser();
}

std::string MicMonitor::currentUid() {
  PwLock lock(engine_);
  return selector_.current();
}

void MicMonitor::onGraphChanged() { syncPending_ = true; }

void MicMonitor::onConnectionLost() {
  if (!meters_.empty()) engine_.log("info", "[MicMonitor] PipeWire connection lost, capture paused");
  meters_.clear();
  failureSeen_.clear();
  order_.clear();
  frozen_ = true;
}

void MicMonitor::emit(const std::string& type, const std::string& uid, const std::string& name) {
  sink_.deviceEvent(type, uid, name);
}

std::vector<DeviceLevel> MicMonitor::levels() const {
  std::vector<DeviceLevel> out;
  out.reserve(order_.size());
  for (const std::string& uid : order_) {
    auto it = meters_.find(uid);
    if (it == meters_.end()) continue;
    out.push_back({uid, it->second->name(), it->second->level()});
  }
  return out;
}

void MicMonitor::syncDevices() {
  const int64_t now = steadyNowMs();
  std::vector<DeviceNode> all = engine_.devices("Audio/Source");
  std::vector<DeviceNode> allowed;
  for (DeviceNode& d : all) {
    DeviceVerdict v = classifyDevice(d, DeviceDirection::Capture);
    if (!v.allowed) {
      if (filteredLogged_.insert(d.name).second) {
        engine_.log("debug", "[MicMonitor] Filtered input device: " + d.description + " transport=" + v.transport,
                    {{"uid", d.name}, {"reason", v.reason}});
      }
      continue;
    }
    allowed.push_back(std::move(d));
  }
  const std::string def = engine_.defaultSourceName();
  std::stable_partition(allowed.begin(), allowed.end(), [&](const DeviceNode& d) { return d.name == def; });

  for (auto it = meters_.begin(); it != meters_.end();) {
    const bool keep = std::any_of(allowed.begin(), allowed.end(), [&](const DeviceNode& d) { return d.name == it->first; });
    if (keep) {
      ++it;
      continue;
    }
    engine_.log("info", "[MicMonitor] Device removed: " + it->second->name(), {{"uid", it->first}});
    failureSeen_.erase(it->first);
    it = meters_.erase(it);
  }

  order_.clear();
  for (const DeviceNode& d : allowed) {
    if (!meters_.count(d.name)) {
      auto failed = failedAt_.find(d.name);
      if (failed != failedAt_.end() && now - failed->second < kRetryFailedMs) continue;
      std::unique_ptr<MeterStream> meter(new MeterStream(engine_, MeterStream::Kind::Microphone, d, [this](MeterStream& s) {
        engine_.log("info", "[MicMonitor] Capture started: " + s.name() + " uid=" + s.uid() + " fmt=" + s.formatDescription());
      }));
      std::string error;
      if (!meter->connect(error)) {
        engine_.log("error", "[MicMonitor] Capture FAILED: " + d.description, {{"uid", d.name}, {"error", error}});
        failedAt_[d.name] = now;
        continue;
      }
      failedAt_.erase(d.name);
      engine_.log("info", "[MicMonitor] Device added: " + d.description + " uid=" + d.name + " transport=" +
                              classifyDevice(d, DeviceDirection::Capture).transport);
      meters_[d.name] = std::move(meter);
    }
    order_.push_back(d.name);
  }
}

void MicMonitor::tick() {
  if (!engine_.connected() || !engine_.synced()) {
    frozen_ = true;
    return;
  }
  const int64_t now = steadyNowMs();
  if (frozen_) {
    frozen_ = false;
    syncPending_ = true;
    resumedAtMs_ = now;
  }
  ++tickCount_;
  for (auto it = meters_.begin(); it != meters_.end();) {
    const std::string uid = it->first;
    if (!it->second->failed()) {
      failureSeen_.erase(uid);
      ++it;
      continue;
    }
    // A meter also fails when its device node is unplugged, and the registry
    // can report that removal just after the stream error: a failure is
    // judged on the tick after it was first seen.
    const std::vector<DeviceNode> present = engine_.devices("Audio/Source");
    const bool exists = std::any_of(present.begin(), present.end(), [&](const DeviceNode& d) { return d.name == uid; });
    if (exists && failureSeen_.insert(uid).second) {
      ++it;
      continue;
    }
    failureSeen_.erase(uid);
    if (exists) {
      engine_.log("warn", "[MicMonitor] Device died, capture stopped: " + it->second->name(),
                  {{"uid", uid}, {"reason", it->second->failure()}});
      failedAt_[uid] = now;
    } else {
      engine_.log("info", "[MicMonitor] Device removed: " + it->second->name(), {{"uid", uid}});
    }
    it = meters_.erase(it);
    syncPending_ = true;
  }
  if (syncPending_ || tickCount_ % kSyncEveryTicks == 0) {
    syncPending_ = false;
    syncDevices();
  }

  const std::vector<DeviceLevel> lv = levels();
  if (resumedAtMs_ > 0 && now - resumedAtMs_ < kResumeGraceMs) {
    // After a (re)connection the session manager re-announces devices one by
    // one: neither report "no device" nor fall back while the selected
    // device may still come back.
    const std::string cur = selector_.current();
    const bool curPresent = std::any_of(lv.begin(), lv.end(), [&](const DeviceLevel& d) { return d.uid == cur; });
    if (lv.empty() || (!cur.empty() && !curPresent)) return;
  }
  if (!lv.empty()) {
    const std::string cur = selector_.current();
    const bool present = std::any_of(lv.begin(), lv.end(), [&](const DeviceLevel& d) { return d.uid == cur; });
    if (!present) {
      const DeviceLevel& fb = lv.front();
      selector_.setCurrent(fb.uid);
      engine_.log("warn",
                  (cur.empty() ? "[MicMonitor] Input device available, fallback to: " : "[MicMonitor] Device lost, fallback to: ") +
                      fb.name,
                  {{"uid", fb.uid}});
      emit("deviceFallbackRequired", fb.uid, fb.name);
    }
  }

  std::vector<VoiceSelector::Event> events;
  selector_.evaluate(now, lv, events);
  for (const VoiceSelector::Event& e : events) {
    switch (e.type) {
      case VoiceSelector::EventType::AutoSelected:
        engine_.log("info", "[MicMonitor] Switch \xE2\x86\x92 " + e.name + " uid=" + e.uid);
        emit("deviceAutoSelected", e.uid, e.name);
        break;
      case VoiceSelector::EventType::WeakVoice:
        engine_.log("warn", "[MicMonitor] Weak voice on ALL devices for 10s");
        emit("weakVoice", "", "");
        break;
      case VoiceSelector::EventType::WeakVoiceRecovered:
        engine_.log("info", "[MicMonitor] Voice recovered on " + e.name);
        emit("weakVoiceRecovered", e.uid, e.name);
        break;
      case VoiceSelector::EventType::NoDevice:
        engine_.log("warn", "[MicMonitor] No physical input device available");
        selector_.setCurrent("");
        emit("noDevice", "", "");
        break;
    }
  }
}

}  // namespace am
