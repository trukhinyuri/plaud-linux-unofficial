// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// xdg-desktop-portal Screenshot (org.freedesktop.portal.Screenshot) over a
// private libdbus-1 session-bus connection.

#pragma once

#include <cstdint>
#include <string>

namespace plaud_rec {
namespace screen {

// Reads the Screenshot interface version; false when the portal (or a
// backend implementing Screenshot) is not available on the session bus.
bool PortalScreenshotVersion(uint32_t& version, std::string& error);

// Requests a non-interactive full-desktop screenshot and waits for the
// Request::Response signal. On success `file_path` is the local path of the
// PNG the portal backend wrote.
bool PortalScreenshot(std::string& file_path, std::string& error, int timeout_ms);

}  // namespace screen
}  // namespace plaud_rec
