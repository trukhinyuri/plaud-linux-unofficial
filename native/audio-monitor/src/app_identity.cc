// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "app_identity.h"

#include <dirent.h>

#include <cstdlib>
#include <sstream>

namespace am {

namespace {

int toPid(const std::string& s) {
  if (s.empty()) return 0;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || v <= 0 || v > (1L << 30)) return 0;
  return static_cast<int>(v);
}

bool processExists(int pid) {
  int ppid = 0;
  uint64_t start = 0;
  return pid > 0 && readProcStat(pid, ppid, start);
}

// Innermost pid of `hostPid` according to /proc/<pid>/status "NSpid:".
int innermostNsPid(int hostPid) {
  std::string status = readSmallFile("/proc/" + std::to_string(hostPid) + "/status", 16 * 1024);
  size_t pos = status.find("\nNSpid:");
  if (pos == std::string::npos) return 0;
  size_t end = status.find('\n', pos + 1);
  std::istringstream line(status.substr(pos + 7, end == std::string::npos ? std::string::npos : end - pos - 7));
  int value = 0;
  int last = 0;
  while (line >> value) last = value;
  return last;
}

bool liveSelfOrDescendant(int pid, int ancestor) {
  for (int guard = 0; pid > 0 && guard < 512; ++guard) {
    if (pid == ancestor) return true;
    int ppid = 0;
    uint64_t start = 0;
    if (!readProcStat(pid, ppid, start) || ppid == pid) return false;
    pid = ppid;
  }
  return false;
}

std::string displayExe(const ProcInfo& info) { return info.exeName.empty() ? info.comm : info.exeName; }

// Chromium/Electron child processes (renderer, zygote, utility, gpu) carry
// "--type=<kind>"; the browser/main process does not.
bool isChromiumChild(const ProcInfo& info) {
  for (size_t i = 1; i < info.argv.size(); ++i) {
    if (startsWith(info.argv[i], "--type=")) return true;
  }
  return false;
}

}  // namespace

bool isHelperExecutable(const std::string& exeName) {
  static const char* const kHelpers[] = {"WebKitWebProcess", "WebKitGPUProcess", "WebKitNetworkProcess",
                                         "QtWebEngineProcess", "plugin-container"};
  for (const char* h : kHelpers) {
    if (exeName == h) return true;
  }
  return false;
}

const CatalogEntry* IdentityResolver::entryForProcess(const ProcInfo& info) {
  if (const CatalogEntry* e = catalogBySandboxId(info.flatpakId)) return e;
  if (const CatalogEntry* e = catalogBySandboxId(info.snapName)) return e;
  if (!isGenericRuntime(info.exeName)) {
    if (const CatalogEntry* e = catalogByBinary(info.exeName)) return e;
  }
  if (!isGenericRuntime(info.comm)) {
    if (const CatalogEntry* e = catalogByComm(info.comm)) return e;
  }
  if (const CatalogEntry* e = catalogBySandboxId(info.desktopId)) return e;
  if (const CatalogEntry* e = catalogByPathMarker(info.exePath)) return e;
  // Generic runtimes (system Electron, node...) carry the app path in argv.
  if (isGenericRuntime(info.exeName) || info.exeName.empty()) {
    for (size_t i = 1; i < info.argv.size(); ++i) {
      if (startsWith(info.argv[i], "--")) continue;
      if (const CatalogEntry* e = catalogByPathMarker(info.argv[i])) return e;
    }
  }
  return nullptr;
}

int IdentityResolver::resolvePid(const Props& nodeProps, const Props& clientProps, const std::string& flatpakHint,
                                 const std::string& binaryHint) const {
  const bool pulseClient = prop(clientProps, "client.api") == "pipewire-pulse" ||
                           prop(nodeProps, "client.api") == "pipewire-pulse";
  const bool flatpakClient = !flatpakHint.empty() || prop(clientProps, "pipewire.access") == "flatpak";
  int pid = 0;
  if (!pulseClient) {
    // Native protocol: the server stamped the peer's host pid.
    pid = toPid(prop(clientProps, "pipewire.sec.pid"));
    if (pid <= 0) pid = toPid(prop(nodeProps, "application.process.id"));
    return processExists(pid) ? pid : 0;
  }
  const int reportedPid = toPid(propOr(nodeProps, "application.process.id", prop(clientProps, "application.process.id")));
  if (!flatpakClient) return processExists(reportedPid) ? reportedPid : 0;

  // libpulse inside Flatpak reports its namespaced pid: map it back to the
  // host pid via the instance cgroup and NSpid.
  std::vector<int> candidates;
  if (!flatpakHint.empty()) {
    candidates = pidsOfFlatpakApp(flatpakHint);
  } else {
    DIR* dir = ::opendir("/proc");
    if (dir) {
      while (struct dirent* ent = ::readdir(dir)) {
        int p = toPid(ent->d_name);
        if (p <= 0) continue;
        std::string cgroup = readSmallFile("/proc/" + std::to_string(p) + "/cgroup", 16 * 1024);
        if (!flatpakIdFromCgroup(cgroup).empty()) candidates.push_back(p);
      }
      ::closedir(dir);
    }
  }
  int byBinary = 0;
  for (int candidate : candidates) {
    if (reportedPid > 0 && innermostNsPid(candidate) == reportedPid) {
      ProcInfo info;
      if (binaryHint.empty() || !readProcInfo(candidate, info) || iequals(displayExe(info), binaryHint) ||
          iequals(info.comm, binaryHint.substr(0, 15))) {
        return candidate;
      }
    }
    if (byBinary == 0 && !binaryHint.empty()) {
      ProcInfo info;
      if (readProcInfo(candidate, info) && (iequals(displayExe(info), binaryHint) || iequals(info.comm, binaryHint)))
        byBinary = candidate;
    }
  }
  return byBinary;
}

AppIdentity IdentityResolver::resolve(const Props& nodeProps, const Props& clientProps,
                                      const ProcessTable& table) const {
  AppIdentity id;
  const std::string nodeBinary = prop(nodeProps, "application.process.binary");
  const std::string nodeAppName = prop(nodeProps, "application.name");
  const std::string nodeAppId = prop(nodeProps, "application.id");
  const std::string clientBinary = prop(clientProps, "application.process.binary");
  const std::string clientAppName = prop(clientProps, "application.name");
  std::string flatpakHint = prop(nodeProps, "pipewire.access.portal.app_id");
  if (flatpakHint.empty()) flatpakHint = prop(clientProps, "pipewire.access.portal.app_id");

  id.pid = resolvePid(nodeProps, clientProps, flatpakHint, nodeBinary.empty() ? clientBinary : nodeBinary);

  ProcInfo info;
  const bool haveInfo = id.pid > 0 && readProcInfo(id.pid, info);
  if (!haveInfo) id.pid = 0;

  if (id.pid > 0 && (table.isSelfOrDescendant(id.pid, selfPid_) || liveSelfOrDescendant(id.pid, selfPid_))) {
    id.isSelf = true;
  }

  // WebKit/QtWebEngine helpers capture on behalf of their UI process.
  ProcInfo appInfo = info;
  for (int guard = 0; haveInfo && guard < 8 && isHelperExecutable(displayExe(appInfo)); ++guard) {
    ProcInfo parent;
    if (!readProcInfo(appInfo.ppid, parent)) break;
    appInfo = parent;
  }

  if (isAudioDaemon(nodeBinary) || (haveInfo && (isAudioDaemon(info.exeName) || isAudioDaemon(info.comm)))) {
    id.isSystem = true;
  }
  if (!haveInfo && nodeBinary.empty() && nodeAppName.empty() && clientBinary.empty()) {
    // Server-side module node (loopback, filter-chain, echo-cancel) with no
    // client application behind it.
    id.isSystem = true;
  }

  id.flatpakId = !flatpakHint.empty() ? flatpakHint : appInfo.flatpakId;
  id.snapName = appInfo.snapName;

  auto tryMatch = [&id](const CatalogEntry* e, const char* by) {
    if (id.entry == nullptr && e != nullptr) {
      id.entry = e;
      id.matchedBy = by;
    }
  };
  tryMatch(catalogBySandboxId(id.flatpakId), "flatpak");
  tryMatch(catalogBySandboxId(id.snapName), "snap");
  if (!isGenericRuntime(nodeBinary)) tryMatch(catalogByBinary(nodeBinary), "application.process.binary");
  tryMatch(catalogByAppName(nodeAppName), "application.name");
  tryMatch(catalogBySandboxId(nodeAppId), "application.id");
  if (haveInfo) tryMatch(entryForProcess(appInfo), "process");
  if (!isGenericRuntime(clientBinary)) tryMatch(catalogByBinary(clientBinary), "client.application.process.binary");
  tryMatch(catalogByAppName(clientAppName), "client.application.name");

  // Root of the application's process tree: climb while the parent belongs
  // to the same app (same catalog entry, same executable, or same Flatpak).
  if (haveInfo) {
    ProcInfo current = appInfo;
    for (int guard = 0; guard < 64; ++guard) {
      if (current.ppid <= 1) break;
      ProcInfo parent;
      if (!readProcInfo(current.ppid, parent)) break;
      const CatalogEntry* parentEntry = entryForProcess(parent);
      const bool sameEntry = id.entry != nullptr && parentEntry == id.entry;
      const bool sameExe = !current.exePath.empty() && parent.exePath == current.exePath &&
                           !isGenericRuntime(parent.exeName);
      // System Electron (exe "electron"): only climb from a Chromium child
      // process to its main process, never past the main process.
      const bool sameElectronApp = !current.exePath.empty() && parent.exePath == current.exePath &&
                                   isGenericRuntime(parent.exeName) && isChromiumChild(current);
      const bool sameFlatpak = !id.flatpakId.empty() && parent.flatpakId == id.flatpakId;
      if (!(sameEntry || sameExe || sameElectronApp || sameFlatpak)) break;
      current = parent;
    }
    id.rootPid = current.pid;
    if (id.entry == nullptr && current.pid != appInfo.pid) tryMatch(entryForProcess(current), "root-process");
  }

  if (!nodeBinary.empty()) {
    id.processName = nodeBinary;
  } else if (haveInfo) {
    id.processName = displayExe(appInfo);
  } else {
    id.processName = clientBinary;
  }

  if (id.entry != nullptr) {
    id.appName = id.entry->appName;
  } else if (!nodeBinary.empty() && !isGenericRuntime(nodeBinary)) {
    id.appName = nodeBinary;
  } else if (!nodeAppName.empty()) {
    id.appName = nodeAppName;
  } else if (haveInfo && !displayExe(appInfo).empty() && !isGenericRuntime(displayExe(appInfo))) {
    id.appName = displayExe(appInfo);
  } else if (!clientAppName.empty()) {
    id.appName = clientAppName;
  } else if (!clientBinary.empty()) {
    id.appName = clientBinary;
  } else if (haveInfo && !appInfo.comm.empty()) {
    id.appName = appInfo.comm;
  } else {
    id.appName = "unknown";
  }
  return id;
}

}  // namespace am
