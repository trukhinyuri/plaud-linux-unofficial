// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Top-level window titles, the secondary browser-meeting signal (the macOS
// addon has no title path; on Linux the title of a browser window showing a
// meeting service is used when the browser tree has no WebRTC UDP socket).
//
// Providers, chosen once per process from the session environment:
//   1. PLAUD_AUDIO_MONITOR_WINDOW_LIST=<file>: JSON array of
//      {"pid":N,"appId":"...","title":"..."} re-read on every query
//      (diagnostics, tests, and compositors without a supported IPC);
//   2. Hyprland IPC  ($HYPRLAND_INSTANCE_SIGNATURE, request "j/clients");
//   3. Sway / i3 IPC ($SWAYSOCK / $I3SOCK, GET_TREE);
//   4. X11 EWMH      ($DISPLAY, _NET_CLIENT_LIST + _NET_WM_PID + _NET_WM_NAME,
//      libxcb loaded with dlopen, so no build/runtime dependency).
// GNOME Shell and KDE Plasma on Wayland expose no window list to clients;
// there only native Wayland windows are invisible and the UDP evidence alone
// decides (XWayland windows remain visible through X11).
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace am {

struct WindowInfo {
  int pid = 0;
  std::string appId;  // Wayland app_id / X11 WM_CLASS class
  std::string title;
};

class WindowTitleProvider {
 public:
  virtual ~WindowTitleProvider() = default;
  virtual std::string name() const = 0;
  // Fills `out`; false when the provider is currently unusable.
  virtual bool list(std::vector<WindowInfo>& out) = 0;
};

std::unique_ptr<WindowTitleProvider> makeFileWindowProvider(const std::string& path);
std::unique_ptr<WindowTitleProvider> makeHyprlandWindowProvider();  // nullptr outside Hyprland
std::unique_ptr<WindowTitleProvider> makeSwayWindowProvider();      // nullptr outside Sway/i3
std::unique_ptr<WindowTitleProvider> makeX11WindowProvider();       // nullptr without DISPLAY/libxcb
// First available provider in the order documented above; nullptr if none.
std::unique_ptr<WindowTitleProvider> makeDefaultWindowProvider();

// Meeting service shown by a window title ("google-meet", "teams", ...), or
// "" when the title does not look like a meeting.
std::string meetingServiceFromTitle(const std::string& title);

}  // namespace am
