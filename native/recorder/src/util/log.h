// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#pragma once

#include <functional>
#include <string>

namespace plaud_rec {

// Level strings are the ones the official addons emit in {type:"log", level}.
enum class LogLevel { Debug, Info, Warning, Error };

inline const char* LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warning:
      return "warning";
    case LogLevel::Error:
      return "error";
  }
  return "info";
}

using LogFn = std::function<void(LogLevel, const std::string&)>;

}  // namespace plaud_rec
