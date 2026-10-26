// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// WebRTC media evidence for browsers, the Linux analog of the macOS addon's
// UDP-bind check (listUdpBindsForPid): a browser that captures the
// microphone counts as "in a meeting" only while its process tree holds an
// ICE/media UDP socket.
//
// WebRTC ICE sockets are *unconnected* UDP sockets (sendto() to many
// candidates) bound to an interface address or the wildcard address on an
// ephemeral port. QUIC/DNS sockets are connected, mDNS/SSDP/LLMNR use fixed
// well-known ports or multicast groups; those are rejected.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace am {

struct UdpSocketEntry {
  bool v6 = false;
  uint8_t localAddr[16] = {};
  uint16_t localPort = 0;
  uint8_t remoteAddr[16] = {};
  uint16_t remotePort = 0;
  unsigned state = 0;  // 7 (TCP_CLOSE) = unconnected, 1 (TCP_ESTABLISHED) = connected
  uint64_t inode = 0;
};

// Parses one data line of /proc/<pid>/net/udp or udp6.
bool parseProcNetUdpLine(const std::string& line, bool v6, UdpSocketEntry& out);

enum class UdpVerdict { Accepted, Loopback, Multicast, Connected, ReservedPort };
UdpVerdict classifyUdpSocket(const UdpSocketEntry& entry);
std::string formatUdpEndpoint(const UdpSocketEntry& entry);

struct UdpScanStats {
  int sockets = 0;
  int accepted = 0;
  int rejLoopback = 0;
  int rejMulticast = 0;
  int rejConnected = 0;
  int rejReservedPort = 0;
  int unreadablePids = 0;
  std::string sampleAccepted;  // "pid:addr:port"
};

class UdpEvidenceScanner {
 public:
  // True when any pid in `pids` owns a WebRTC-like UDP socket.
  bool scan(const std::vector<int>& pids, UdpScanStats* stats);

 private:
  // netns id -> inode -> socket, valid for one scan() call
  std::unordered_map<std::string, std::unordered_map<uint64_t, UdpSocketEntry>> tables_;
  const std::unordered_map<uint64_t, UdpSocketEntry>& tableFor(int pid);
};

}  // namespace am
