// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// X11 access through dlopen("libX11.so.6") / dlopen("libXrandr.so.2"), the
// same libraries Electron itself links. Geometry and window queries work on
// any X server (including XWayland); pixel capture (GrabRoot) is only valid
// in a real X11 session and callers must not use it under Wayland.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "image.h"
#include "screen_log.h"

namespace plaud_rec {
namespace screen {

struct X11Monitor {
  std::string name;  // RandR monitor/output name (XWayland mirrors wl_output names)
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool primary = false;
  uint32_t output_id = 0;  // first RandR output XID; Electron's display.id under X11/XWayland
};

struct X11Window {
  uint32_t id = 0;  // client window XID
  int x = 0;        // outer bounds (frame extents included) in root coordinates
  int y = 0;
  int width = 0;
  int height = 0;
  std::string owner;  // WM_CLASS class
  std::string title;  // _NET_WM_NAME / WM_NAME
};

class X11Connection {
 public:
  // Opens $DISPLAY. Returns nullptr (with `error`) when libX11 or the display
  // is unavailable.
  static std::unique_ptr<X11Connection> Open(std::string& error);
  ~X11Connection();
  X11Connection(const X11Connection&) = delete;
  X11Connection& operator=(const X11Connection&) = delete;

  bool IsXWayland() const;
  void RootSize(int& width, int& height) const;
  // RandR 1.5 monitors, else CRTC-backed outputs, else one root-sized monitor.
  std::vector<X11Monitor> Monitors() const;
  bool QueryPointer(int& x, int& y) const;
  // Topmost normal client window containing the pointer (EWMH stacking order).
  bool WindowUnderPointer(X11Window& out, const Log& log) const;
  bool WindowById(uint32_t id, X11Window& out) const;
  // Root-window pixels; the rectangle must lie within the root window.
  bool GrabRoot(int x, int y, int width, int height, Image& out, std::string& error) const;

 private:
  explicit X11Connection(void* display) : display_(display) {}
  void* display_;
};

}  // namespace screen
}  // namespace plaud_rec
