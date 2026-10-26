// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Helper-process execution for the screen-capture backends: posix_spawn
// without a shell, stdout captured as bytes, stderr as text, hard timeout.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace plaud_rec {
namespace screen {

struct ProcessResult {
  bool started = false;
  bool timed_out = false;
  bool output_truncated = false;
  int exit_code = -1;     // valid when exited normally
  int term_signal = 0;    // non-zero when killed by a signal
  std::vector<uint8_t> out;
  std::string err;        // stderr (bounded)
  std::string spawn_error;

  bool Succeeded() const { return started && !timed_out && term_signal == 0 && exit_code == 0; }
  std::string Describe() const;  // one-line failure summary for logs
};

// Resolves a program name through $PATH (absolute/relative paths are checked
// as given). Returns an empty string when no executable is found.
std::string FindExecutable(const std::string& name);

// Runs argv[0] (an executable path) with argv, inheriting the environment plus
// `extra_env` ("NAME=value" entries override inherited ones). stdin is
// /dev/null. The child is killed (SIGKILL) after timeout_ms.
ProcessResult RunProcess(const std::vector<std::string>& argv, const std::vector<std::string>& extra_env,
                         int timeout_ms, size_t max_stdout_bytes);

}  // namespace screen
}  // namespace plaud_rec
