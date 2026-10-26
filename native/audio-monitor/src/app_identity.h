// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Resolves which application owns a PipeWire capture stream.
//
// Sources, strongest first:
//   * host pid: `pipewire.sec.pid` of native PipeWire clients (kernel
//     SO_PEERCRED, already translated to the host pid namespace); for
//     pipewire-pulse clients the node's `application.process.id` (libpulse
//     getpid()), except inside Flatpak where that pid is namespaced and the
//     host processes are found through the `app-flatpak-<id>-<n>.scope` cgroup;
//   * stream props: application.process.binary, application.name,
//     application.id, pipewire.access.portal.app_id;
//   * /proc: exe, comm, cgroup (Flatpak id, Snap name, XDG desktop id), argv,
//     and the parent chain (helper processes inherit the app of their parent).
#pragma once

#include <string>

#include "app_catalog.h"
#include "common.h"
#include "proc_util.h"

namespace am {

struct AppIdentity {
  bool isSelf = false;    // our own process or one of its descendants
  bool isSystem = false;  // PipeWire/WirePlumber-internal stream
  std::string appName;      // reported appName
  std::string processName;  // raw executable/binary name for meta
  const CatalogEntry* entry = nullptr;
  int pid = 0;      // host pid of the capturing process (0 = unknown)
  int rootPid = 0;  // top-most ancestor that belongs to the same application
  std::string flatpakId;
  std::string snapName;
  std::string matchedBy;  // which identity source decided the entry

  bool isBrowser() const { return entry != nullptr && entry->isBrowser(); }
};

class IdentityResolver {
 public:
  explicit IdentityResolver(int selfPid) : selfPid_(selfPid) {}

  // `table` must be fresh enough to contain the stream's process.
  AppIdentity resolve(const Props& nodeProps, const Props& clientProps, const ProcessTable& table) const;

  // Catalog entry for a process judged only by its own /proc identity.
  static const CatalogEntry* entryForProcess(const ProcInfo& info);

  int selfPid() const { return selfPid_; }

 private:
  int resolvePid(const Props& nodeProps, const Props& clientProps, const std::string& flatpakHint,
                 const std::string& binaryHint) const;
  int selfPid_;
};

// Executables that only host content for a parent UI process (WebKit and
// QtWebEngine helpers); their app is the parent's app.
bool isHelperExecutable(const std::string& exeName);

}  // namespace am
