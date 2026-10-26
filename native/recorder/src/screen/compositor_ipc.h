// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Request/response IPC with Hyprland (.socket.sock, "j/<command>") and sway
// (i3-ipc framing on $SWAYSOCK). Used for cursor position and window lists.

#pragma once

#include <cstdint>
#include <string>

#include "json.h"

namespace plaud_rec {
namespace screen {

// Hyprland control socket: $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/
// .socket.sock (older releases: /tmp/hypr/...). Empty when not found.
std::string HyprlandSocketPath();
bool HyprlandQuery(const std::string& command, Json& out, std::string& error);

// sway IPC message types used here.
constexpr uint32_t kSwayGetOutputs = 3;
constexpr uint32_t kSwayGetTree = 4;
std::string SwaySocketPath();
bool SwayQuery(uint32_t type, Json& out, std::string& error);

}  // namespace screen
}  // namespace plaud_rec
