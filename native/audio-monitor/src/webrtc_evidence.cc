// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "webrtc_evidence.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <sstream>

#include "proc_util.h"

namespace am {

namespace {

bool parseHexU32(const std::string& s, size_t pos, uint32_t& out) {
  if (pos + 8 > s.size()) return false;
  out = 0;
  for (size_t i = 0; i < 8; ++i) {
    char c = s[pos + i];
    out <<= 4;
    if (c >= '0' && c <= '9') {
      out |= static_cast<uint32_t>(c - '0');
    } else if (c >= 'A' && c <= 'F') {
      out |= static_cast<uint32_t>(c - 'A' + 10);
    } else if (c >= 'a' && c <= 'f') {
      out |= static_cast<uint32_t>(c - 'a' + 10);
    } else {
      return false;
    }
  }
  return true;
}

// "ADDR:PORT" where ADDR is 8 (v4) or 32 (v6) hex digits. The kernel prints
// each 32-bit word of the in-memory (network order) address as a host-order
// integer, so copying the parsed integer back to memory restores the bytes.
bool parseEndpoint(const std::string& token, bool v6, uint8_t* addr, uint16_t& port) {
  const size_t addrLen = v6 ? 32 : 8;
  if (token.size() != addrLen + 5 || token[addrLen] != ':') return false;
  for (size_t word = 0; word < addrLen / 8; ++word) {
    uint32_t v = 0;
    if (!parseHexU32(token, word * 8, v)) return false;
    std::memcpy(addr + word * 4, &v, 4);
  }
  char* end = nullptr;
  unsigned long p = std::strtoul(token.substr(addrLen + 1).c_str(), &end, 16);
  if (end == nullptr || *end != '\0' || p > 0xFFFF) return false;
  port = static_cast<uint16_t>(p);
  return true;
}

bool isV4Mapped(const uint8_t* a) {
  static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
  return std::memcmp(a, prefix, 12) == 0;
}

bool allZero(const uint8_t* a, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != 0) return false;
  }
  return true;
}

}  // namespace

bool parseProcNetUdpLine(const std::string& line, bool v6, UdpSocketEntry& out) {
  std::istringstream in(line);
  std::string sl, local, remote, st, txrx, trtm, retr, uid, timeout, inode;
  if (!(in >> sl >> local >> remote >> st >> txrx >> trtm >> retr >> uid >> timeout >> inode)) return false;
  if (sl.empty() || sl.back() != ':') return false;
  UdpSocketEntry e;
  e.v6 = v6;
  if (!parseEndpoint(local, v6, e.localAddr, e.localPort)) return false;
  if (!parseEndpoint(remote, v6, e.remoteAddr, e.remotePort)) return false;
  e.state = static_cast<unsigned>(std::strtoul(st.c_str(), nullptr, 16));
  e.inode = std::strtoull(inode.c_str(), nullptr, 10);
  out = e;
  return true;
}

UdpVerdict classifyUdpSocket(const UdpSocketEntry& e) {
  const uint8_t* a = e.localAddr;
  const uint8_t* v4 = nullptr;
  if (!e.v6) {
    v4 = a;
  } else if (isV4Mapped(a)) {
    v4 = a + 12;
  }
  if (v4 != nullptr) {
    if (v4[0] == 127) return UdpVerdict::Loopback;
    if (v4[0] >= 224 && v4[0] <= 239) return UdpVerdict::Multicast;
  } else {
    static const uint8_t loop6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(a, loop6, 16) == 0) return UdpVerdict::Loopback;
    if (a[0] == 0xFF) return UdpVerdict::Multicast;
  }
  const size_t addrLen = e.v6 ? 16 : 4;
  if (e.remotePort != 0 || !allZero(e.remoteAddr, addrLen) || e.state == 1) return UdpVerdict::Connected;
  // Well-known discovery/name-resolution services that browsers keep open
  // regardless of calls: SSDP 1900, WS-Discovery 3702, mDNS 5353, LLMNR 5355.
  if (e.localPort < 1024 || e.localPort == 1900 || e.localPort == 3702 || e.localPort == 5353 ||
      e.localPort == 5355) {
    return UdpVerdict::ReservedPort;
  }
  return UdpVerdict::Accepted;
}

std::string formatUdpEndpoint(const UdpSocketEntry& e) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (e.v6) {
    inet_ntop(AF_INET6, e.localAddr, buf, sizeof(buf));
    return std::string("[") + buf + "]:" + std::to_string(e.localPort);
  }
  inet_ntop(AF_INET, e.localAddr, buf, sizeof(buf));
  return std::string(buf) + ":" + std::to_string(e.localPort);
}

const std::unordered_map<uint64_t, UdpSocketEntry>& UdpEvidenceScanner::tableFor(int pid) {
  const std::string base = "/proc/" + std::to_string(pid);
  char ns[128] = {};
  ssize_t n = ::readlink((base + "/ns/net").c_str(), ns, sizeof(ns) - 1);
  std::string key = n > 0 ? std::string(ns, static_cast<size_t>(n)) : "pid:" + std::to_string(pid);
  auto it = tables_.find(key);
  if (it != tables_.end()) return it->second;
  auto& table = tables_[key];
  for (int v6 = 0; v6 < 2; ++v6) {
    std::string text = readSmallFile(base + (v6 ? "/net/udp6" : "/net/udp"), 8 * 1024 * 1024);
    size_t start = text.find('\n');  // skip header
    while (start != std::string::npos && start < text.size()) {
      size_t end = text.find('\n', start + 1);
      std::string line = text.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
      UdpSocketEntry e;
      if (parseProcNetUdpLine(line, v6 != 0, e) && e.inode != 0) table[e.inode] = e;
      start = end;
    }
  }
  return table;
}

bool UdpEvidenceScanner::scan(const std::vector<int>& pids, UdpScanStats* stats) {
  tables_.clear();
  UdpScanStats local;
  UdpScanStats& s = stats ? *stats : local;
  for (int pid : pids) {
    const std::string fdDir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* dir = ::opendir(fdDir.c_str());
    if (!dir) {
      ++s.unreadablePids;
      continue;
    }
    const std::unordered_map<uint64_t, UdpSocketEntry>* table = nullptr;
    bool found = false;
    while (struct dirent* ent = ::readdir(dir)) {
      if (ent->d_name[0] == '.') continue;
      char target[64] = {};
      ssize_t n = ::readlinkat(dirfd(dir), ent->d_name, target, sizeof(target) - 1);
      if (n <= 9 || std::strncmp(target, "socket:[", 8) != 0) continue;
      uint64_t inode = std::strtoull(target + 8, nullptr, 10);
      if (table == nullptr) table = &tableFor(pid);
      auto it = table->find(inode);
      if (it == table->end()) continue;  // not a UDP socket
      ++s.sockets;
      switch (classifyUdpSocket(it->second)) {
        case UdpVerdict::Accepted:
          ++s.accepted;
          if (s.sampleAccepted.empty()) s.sampleAccepted = std::to_string(pid) + ":" + formatUdpEndpoint(it->second);
          found = true;
          break;
        case UdpVerdict::Loopback:
          ++s.rejLoopback;
          break;
        case UdpVerdict::Multicast:
          ++s.rejMulticast;
          break;
        case UdpVerdict::Connected:
          ++s.rejConnected;
          break;
        case UdpVerdict::ReservedPort:
          ++s.rejReservedPort;
          break;
      }
      if (found) break;
    }
    ::closedir(dir);
    if (found) {
      tables_.clear();
      return true;
    }
  }
  tables_.clear();
  return false;
}

}  // namespace am
