// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Logging sink of the screen-capture statics. The official screen code paths
// emit the level strings "info", "warn" and "error" (not "warning"), so this
// slice keeps its own string-level sink instead of util/log.h.

#pragma once

#include <functional>
#include <string>
#include <utility>

namespace plaud_rec {
namespace screen {

using LogSink = std::function<void(const std::string& level, const std::string& message)>;

class Log {
 public:
  Log() = default;
  explicit Log(LogSink sink, std::string prefix = std::string())
      : sink_(std::move(sink)), prefix_(std::move(prefix)) {}

  void Info(const std::string& message) const { Emit("info", message); }
  void Warn(const std::string& message) const { Emit("warn", message); }
  void Error(const std::string& message) const { Emit("error", message); }

 private:
  void Emit(const char* level, const std::string& message) const {
    if (sink_) sink_(level, prefix_.empty() ? message : prefix_ + message);
  }

  LogSink sink_;
  std::string prefix_;
};

}  // namespace screen
}  // namespace plaud_rec
