// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// /proc helpers: process identity (exe, comm, argv, cgroup-derived sandbox
// ids), process-tree snapshots and small string utilities.
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace am {

struct ProcInfo {
  int pid = 0;
  int ppid = 0;
  uint64_t startTime = 0;  // /proc/<pid>/stat field 22 (clock ticks since boot)
  uid_t uid = static_cast<uid_t>(-1);
  std::string comm;      // /proc/<pid>/comm (kernel truncates to 15 bytes)
  std::string exePath;   // readlink(/proc/<pid>/exe), empty when not permitted
  std::string exeName;   // basename(exePath) without " (deleted)"
  std::vector<std::string> argv;
  std::string flatpakId;  // from cgroup app-flatpak-<id>-<n>.scope
  std::string snapName;   // from cgroup snap.<name>.<app>-<uuid>.scope
  std::string desktopId;  // from cgroup app-<launcher>-<desktop id>-<n>.scope / @<n>.service
};

// Reads identity of `pid`. Returns false when the process does not exist.
bool readProcInfo(int pid, ProcInfo& out);

// Parses /proc/<pid>/stat. Returns false when the process does not exist.
bool readProcStat(int pid, int& ppid, uint64_t& startTime);

// Reads a small text file completely (up to `limit` bytes). Empty on error.
std::string readSmallFile(const std::string& path, size_t limit = 256 * 1024);

// cgroup v1/v2 content -> sandbox ids ("" when not applicable).
std::string flatpakIdFromCgroup(const std::string& cgroup);
std::string snapNameFromCgroup(const std::string& cgroup);
std::string desktopIdFromCgroup(const std::string& cgroup);

// Snapshot of the whole process table (pid -> ppid), refreshed on demand.
class ProcessTable {
 public:
  void refresh();
  int parentOf(int pid) const;
  bool contains(int pid) const { return parents_.count(pid) != 0; }
  // `root` and all its descendants (breadth first, root first).
  std::vector<int> subtree(int root) const;
  // True when `pid` == `ancestor` or `ancestor` is on pid's parent chain.
  bool isSelfOrDescendant(int pid, int ancestor) const;
  const std::unordered_map<int, int>& parents() const { return parents_; }

 private:
  std::unordered_map<int, int> parents_;
  std::unordered_map<int, std::vector<int>> children_;
};

// All live pids whose cgroup places them in the Flatpak instance of `appId`.
std::vector<int> pidsOfFlatpakApp(const std::string& appId);

std::string baseName(const std::string& path);
std::string toLower(std::string s);
bool iequals(const std::string& a, const std::string& b);
bool icontains(const std::string& haystack, const std::string& needle);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);

}  // namespace am
