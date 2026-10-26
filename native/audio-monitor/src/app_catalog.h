// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Catalog of meeting-capable Linux applications and browsers.
//
// Each entry maps Linux identities (executable names, Flatpak/Snap ids,
// desktop ids, PipeWire `application.name`, install-path markers) to the
// `appName` the addon reports. Every reported appName contains the key the
// Plaud JS matches with `appName.toLowerCase().includes(key)` (zoom, ms-teams,
// slack, webex, lark, feishu, discord, chrome, msedge, firefox, safari, ...),
// and Chromium-family browsers report a name containing "chrome" so the JS
// treats them as a browser (its browser list is chrome/firefox/arc/safari/msedge).
#pragma once

#include <string>
#include <vector>

namespace am {

enum class BrowserEngine { None, Chromium, Gecko, WebKit };

struct CatalogEntry {
  std::string appName;  // reported appName
  BrowserEngine engine = BrowserEngine::None;
  std::vector<std::string> binaries;     // exact executable names (case-insensitive)
  std::vector<std::string> sandboxIds;   // Flatpak ids, Snap names, desktop ids (case-insensitive)
  std::vector<std::string> appNames;     // PipeWire/Pulse application.name (case-insensitive)
  std::vector<std::string> pathMarkers;  // substrings of the executable path or argv (generic runtimes)

  bool isBrowser() const { return engine != BrowserEngine::None; }
};

const std::vector<CatalogEntry>& appCatalog();

const CatalogEntry* catalogByBinary(const std::string& binary);
// Kernel `comm` is truncated to 15 bytes; a 15-byte comm matches by prefix.
const CatalogEntry* catalogByComm(const std::string& comm);
const CatalogEntry* catalogBySandboxId(const std::string& id);
const CatalogEntry* catalogByAppName(const std::string& applicationName);
const CatalogEntry* catalogByPathMarker(const std::string& pathOrArg);

// Runtimes that host many different apps (system Electron, node, python...):
// their executable name says nothing about the app.
bool isGenericRuntime(const std::string& binary);

// PipeWire/WirePlumber daemons: streams they own are graph-internal
// (loopback, echo-cancel, remap/virtual sources), never user apps.
bool isAudioDaemon(const std::string& binary);

const char* engineName(BrowserEngine engine);

}  // namespace am
