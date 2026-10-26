// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Screen-capture backend behind the captureScreenImage / getWindowAtCursor /
// permission statics. Wayland sessions capture through grim (wlr-screencopy
// or ext-image-copy-capture) with the xdg-desktop-portal Screenshot API as
// fallback; X11 sessions read the root window with XGetImage.
//
// Environment overrides:
//   PLAUD_SCREEN_BACKEND=grim|portal|x11  use exactly one capture backend
//   PLAUD_SCREEN_COORDS=x11|wayland       coordinate space of `rect` requests

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "screen_log.h"

namespace plaud_rec {
namespace screen {

// Values of the official ScreenCapturePermissionStatus enum.
enum class PermissionStatus : int { Granted = 0, Denied = 1 };

// Mirrors the official request kinds (dispatch order of the native wrapper).
struct CaptureRequest {
  enum Kind : uint32_t {
    kCursorDisplay = 0,
    kMainDisplay = 1,
    kCursorWindow = 2,
    kDisplayById = 3,
    kDisplayByIndex = 4,
    kWindowById = 5,
    kRect = 6,
  };
  uint32_t kind = kCursorDisplay;
  uint32_t display_id = 0;
  uint32_t display_index = 0;
  uint32_t window_id = 0;
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

struct WindowInfo {
  uint32_t window_id = 0;
  double x = 0;  // bounds in the rect coordinate space of this process
  double y = 0;
  double width = 0;
  double height = 0;
  std::string owner_name;
  std::string window_name;
};

struct HostInfo {
  int electron_major = 0;  // process.versions.electron major, 0 outside Electron
};

// Holds the per-call state (desktop layout, captured outputs) so that a batch
// of requests captures every output at most once.
class ScreenCapturer {
 public:
  explicit ScreenCapturer(const HostInfo& host);
  ~ScreenCapturer();
  ScreenCapturer(const ScreenCapturer&) = delete;
  ScreenCapturer& operator=(const ScreenCapturer&) = delete;

  // PNG bytes, or an empty vector after logging the reason.
  std::vector<uint8_t> Capture(const CaptureRequest& request, const Log& log);
  bool WindowAtCursor(WindowInfo& out, const Log& log);
  // Whether the setup a capture would use succeeds and offers a working
  // backend; `reason` explains a false result. Captures no pixels.
  bool Usable(std::string& reason);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Linux has no screen-recording permission gate: Granted exactly when a
// capture backend is usable (see ScreenCapturer::Usable).
PermissionStatus CheckScreenCapturePermission(const HostInfo& host, const Log& log);

}  // namespace screen
}  // namespace plaud_rec
