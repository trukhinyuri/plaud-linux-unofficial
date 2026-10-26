// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#ifndef AUDIO_MONITOR_VERSION
#define AUDIO_MONITOR_VERSION "1.0.0"
#endif

namespace am {

using Props = std::map<std::string, std::string>;

inline const std::string& propOr(const Props& props, const std::string& key, const std::string& fallback) {
  auto it = props.find(key);
  return it == props.end() ? fallback : it->second;
}

inline std::string prop(const Props& props, const std::string& key) {
  auto it = props.find(key);
  return it == props.end() ? std::string() : it->second;
}

inline int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Log sink shared by every component: level is "debug" | "info" | "warn" | "error".
using LogFn = std::function<void(const std::string& level, const std::string& message, const Props& meta)>;

}  // namespace am
