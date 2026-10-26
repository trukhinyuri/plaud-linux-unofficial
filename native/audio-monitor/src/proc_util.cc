// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "proc_util.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>

namespace am {

namespace {

bool isNumeric(const char* s) {
  if (*s == '\0') return false;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return false;
  }
  return true;
}

// Last path component of every cgroup line ("0::/a/b/leaf.scope" -> leaf.scope).
std::vector<std::string> cgroupLeaves(const std::string& cgroup) {
  std::vector<std::string> leaves;
  size_t start = 0;
  while (start < cgroup.size()) {
    size_t end = cgroup.find('\n', start);
    if (end == std::string::npos) end = cgroup.size();
    std::string line = cgroup.substr(start, end - start);
    size_t slash = line.rfind('/');
    if (slash != std::string::npos && slash + 1 < line.size()) leaves.push_back(line.substr(slash + 1));
    start = end + 1;
  }
  return leaves;
}

// All path components of every cgroup line.
std::vector<std::string> cgroupComponents(const std::string& cgroup) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < cgroup.size()) {
    size_t end = cgroup.find('\n', start);
    if (end == std::string::npos) end = cgroup.size();
    std::string line = cgroup.substr(start, end - start);
    size_t colon = line.rfind(':');
    std::string path = colon == std::string::npos ? line : line.substr(colon + 1);
    size_t p = 0;
    while (p < path.size()) {
      size_t q = path.find('/', p);
      if (q == std::string::npos) q = path.size();
      if (q > p) parts.push_back(path.substr(p, q - p));
      p = q + 1;
    }
    start = end + 1;
  }
  return parts;
}

std::string unescapeSystemd(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x' && std::isxdigit(static_cast<unsigned char>(s[i + 2])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 3]))) {
      out.push_back(static_cast<char>(std::strtol(s.substr(i + 2, 2).c_str(), nullptr, 16)));
      i += 3;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

// Parses systemd XDG application unit names:
//   app[-<launcher>]-<ApplicationID>-<RANDOM>.scope
//   app[-<launcher>]-<ApplicationID>[@<RANDOM>].service
// Returns {launcher, application id}; both empty when `leaf` is not such a unit.
std::pair<std::string, std::string> parseAppUnit(const std::string& leaf) {
  if (!startsWith(leaf, "app-")) return {};
  std::string body;
  if (endsWith(leaf, ".scope")) {
    body = leaf.substr(4, leaf.size() - 4 - 6);
    size_t dash = body.rfind('-');
    if (dash == std::string::npos) return {};
    body = body.substr(0, dash);  // drop <RANDOM>
  } else if (endsWith(leaf, ".service")) {
    body = leaf.substr(4, leaf.size() - 4 - 8);
    size_t at = body.find('@');
    if (at != std::string::npos) body = body.substr(0, at);
  } else {
    return {};
  }
  if (body.empty()) return {};
  // Application IDs are escaped (\x2d for '-'), so the first unescaped '-'
  // separates the optional launcher from the application id.
  size_t dash = body.find('-');
  if (dash == std::string::npos) return {"", unescapeSystemd(body)};
  return {body.substr(0, dash), unescapeSystemd(body.substr(dash + 1))};
}

}  // namespace

std::string readSmallFile(const std::string& path, size_t limit) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::string();
  std::string out;
  char buf[4096];
  while (out.size() < limit) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  if (out.size() > limit) out.resize(limit);
  return out;
}

bool readProcStat(int pid, int& ppid, uint64_t& startTime) {
  std::string stat = readSmallFile("/proc/" + std::to_string(pid) + "/stat", 4096);
  if (stat.empty()) return false;
  size_t close = stat.rfind(')');
  if (close == std::string::npos) return false;
  // Fields after the comm: index 0 = state (field 3), 1 = ppid (field 4),
  // 19 = starttime (field 22).
  const char* p = stat.c_str() + close + 1;
  int field = 0;
  ppid = 0;
  startTime = 0;
  while (*p) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char* tokenStart = p;
    while (*p && *p != ' ') ++p;
    if (field == 1) ppid = std::atoi(std::string(tokenStart, p).c_str());
    if (field == 19) {
      startTime = std::strtoull(std::string(tokenStart, p).c_str(), nullptr, 10);
      break;
    }
    ++field;
  }
  return field >= 1;
}

bool readProcInfo(int pid, ProcInfo& out) {
  if (pid <= 0) return false;
  const std::string base = "/proc/" + std::to_string(pid);
  ProcInfo info;
  info.pid = pid;
  if (!readProcStat(pid, info.ppid, info.startTime)) return false;
  struct stat st {};
  if (::stat(base.c_str(), &st) == 0) info.uid = st.st_uid;
  info.comm = readSmallFile(base + "/comm", 256);
  while (!info.comm.empty() && (info.comm.back() == '\n' || info.comm.back() == '\0')) info.comm.pop_back();
  char exe[4096];
  ssize_t n = ::readlink((base + "/exe").c_str(), exe, sizeof(exe) - 1);
  if (n > 0) {
    info.exePath.assign(exe, static_cast<size_t>(n));
    const std::string deleted = " (deleted)";
    if (endsWith(info.exePath, deleted)) info.exePath.resize(info.exePath.size() - deleted.size());
    info.exeName = baseName(info.exePath);
  }
  std::string cmdline = readSmallFile(base + "/cmdline", 64 * 1024);
  size_t start = 0;
  while (start < cmdline.size()) {
    size_t end = cmdline.find('\0', start);
    if (end == std::string::npos) end = cmdline.size();
    if (end > start) info.argv.push_back(cmdline.substr(start, end - start));
    start = end + 1;
  }
  std::string cgroup = readSmallFile(base + "/cgroup", 16 * 1024);
  info.flatpakId = flatpakIdFromCgroup(cgroup);
  info.snapName = snapNameFromCgroup(cgroup);
  info.desktopId = desktopIdFromCgroup(cgroup);
  out = std::move(info);
  return true;
}

std::string flatpakIdFromCgroup(const std::string& cgroup) {
  for (const std::string& leaf : cgroupLeaves(cgroup)) {
    auto unit = parseAppUnit(leaf);
    if (unit.first == "flatpak" && !unit.second.empty()) return unit.second;
  }
  return std::string();
}

std::string snapNameFromCgroup(const std::string& cgroup) {
  for (const std::string& part : cgroupComponents(cgroup)) {
    if (!startsWith(part, "snap.")) continue;
    size_t dot = part.find('.', 5);
    if (dot == std::string::npos || dot == 5) continue;
    return part.substr(5, dot - 5);
  }
  return std::string();
}

std::string desktopIdFromCgroup(const std::string& cgroup) {
  for (const std::string& leaf : cgroupLeaves(cgroup)) {
    auto unit = parseAppUnit(leaf);
    if (!unit.second.empty() && unit.first != "flatpak") return unit.second;
  }
  return std::string();
}

void ProcessTable::refresh() {
  parents_.clear();
  children_.clear();
  DIR* dir = ::opendir("/proc");
  if (!dir) return;
  while (struct dirent* ent = ::readdir(dir)) {
    if (!isNumeric(ent->d_name)) continue;
    int pid = std::atoi(ent->d_name);
    int ppid = 0;
    uint64_t start = 0;
    if (!readProcStat(pid, ppid, start)) continue;
    parents_[pid] = ppid;
  }
  ::closedir(dir);
  for (const auto& kv : parents_) children_[kv.second].push_back(kv.first);
}

int ProcessTable::parentOf(int pid) const {
  auto it = parents_.find(pid);
  return it == parents_.end() ? 0 : it->second;
}

std::vector<int> ProcessTable::subtree(int root) const {
  std::vector<int> out;
  if (!contains(root)) return out;
  std::deque<int> queue{root};
  while (!queue.empty()) {
    int pid = queue.front();
    queue.pop_front();
    out.push_back(pid);
    auto it = children_.find(pid);
    if (it == children_.end()) continue;
    for (int child : it->second) queue.push_back(child);
  }
  return out;
}

bool ProcessTable::isSelfOrDescendant(int pid, int ancestor) const {
  for (int guard = 0; pid > 0 && guard < 4096; ++guard) {
    if (pid == ancestor) return true;
    int parent = parentOf(pid);
    if (parent == pid) break;
    pid = parent;
  }
  return false;
}

std::vector<int> pidsOfFlatpakApp(const std::string& appId) {
  std::vector<int> pids;
  if (appId.empty()) return pids;
  DIR* dir = ::opendir("/proc");
  if (!dir) return pids;
  while (struct dirent* ent = ::readdir(dir)) {
    if (!isNumeric(ent->d_name)) continue;
    std::string cgroup = readSmallFile(std::string("/proc/") + ent->d_name + "/cgroup", 16 * 1024);
    if (cgroup.empty()) continue;
    if (flatpakIdFromCgroup(cgroup) == appId) pids.push_back(std::atoi(ent->d_name));
  }
  ::closedir(dir);
  std::sort(pids.begin(), pids.end());
  return pids;
}

std::string baseName(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string toLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

bool icontains(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

bool startsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace am
