// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Compositor-independent output layout query. Speaks the Wayland wire
// protocol directly on a private, short-lived connection (no libwayland
// dependency): wl_registry, wl_output (v4) and zxdg_output_manager_v1 (v3).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plaud_rec {
namespace screen {

struct WaylandOutput {
  uint32_t global_name = 0;  // wl_registry name of the wl_output global
  std::string name;          // connector name (wl_output.name v4, else xdg_output.name)
  std::string description;
  int32_t geometry_x = 0;    // wl_output.geometry position
  int32_t geometry_y = 0;
  int32_t transform = 0;     // wl_output.transform enum value
  int32_t scale = 1;         // integer wl_output.scale
  int32_t mode_width = 0;    // current mode, untransformed pixels
  int32_t mode_height = 0;
  bool has_logical = false;  // xdg_output logical geometry received
  int32_t logical_x = 0;
  int32_t logical_y = 0;
  int32_t logical_width = 0;
  int32_t logical_height = 0;
};

struct WaylandInfo {
  std::vector<WaylandOutput> outputs;  // registry order
  bool has_xdg_output_manager = false;
  bool has_wlr_screencopy = false;     // zwlr_screencopy_manager_v1
  bool has_ext_image_copy = false;     // ext_image_copy_capture_manager_v1
};

// Resolves the compositor socket: $WAYLAND_DISPLAY (absolute or relative to
// $XDG_RUNTIME_DIR), else the first wayland-N socket in $XDG_RUNTIME_DIR.
// `display_name` receives the value to export as WAYLAND_DISPLAY to helpers.
bool ResolveWaylandSocket(std::string& socket_path, std::string& display_name);

bool QueryWaylandOutputs(WaylandInfo& out, std::string& error, int timeout_ms);

}  // namespace screen
}  // namespace plaud_rec
