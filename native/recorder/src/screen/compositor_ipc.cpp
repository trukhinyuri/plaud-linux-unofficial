// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "compositor_ipc.h"

#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace plaud_rec {
namespace screen {

namespace {

constexpr int kIpcTimeoutMs = 1500;
constexpr size_t kMaxReplyBytes = 64u << 20;

int64_t NowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool IsSocket(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

class Socket {
 public:
  ~Socket() {
    if (fd_ >= 0) close(fd_);
  }
  bool Connect(const std::string& path, std::string& error) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      error = "IPC socket path too long";
      return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0 || connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      error = "connect(" + path + "): " + std::strerror(errno);
      return false;
    }
    return true;
  }
  bool WriteAll(const void* data, size_t size, int64_t deadline, std::string& error) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
      if (!Wait(POLLOUT, deadline, error)) return false;
      const ssize_t k = send(fd_, p, size, MSG_NOSIGNAL);
      if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (k <= 0) {
        error = std::string("IPC send: ") + std::strerror(errno);
        return false;
      }
      p += k;
      size -= static_cast<size_t>(k);
    }
    return true;
  }
  // Reads exactly `size` bytes, or until EOF when `until_eof` is set.
  bool Read(std::string& out, size_t size, bool until_eof, int64_t deadline, std::string& error) {
    char buf[16384];
    while (until_eof || out.size() < size) {
      if (!Wait(POLLIN, deadline, error)) return false;
      const size_t want = until_eof ? sizeof(buf) : std::min(sizeof(buf), size - out.size());
      const ssize_t k = recv(fd_, buf, want, 0);
      if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (k < 0) {
        error = std::string("IPC recv: ") + std::strerror(errno);
        return false;
      }
      if (k == 0) {
        if (until_eof) return true;
        error = "IPC connection closed early";
        return false;
      }
      out.append(buf, static_cast<size_t>(k));
      if (out.size() > kMaxReplyBytes) {
        error = "IPC reply too large";
        return false;
      }
    }
    return true;
  }

 private:
  bool Wait(short events, int64_t deadline, std::string& error) {
    for (;;) {
      const int64_t remaining = deadline - NowMs();
      if (remaining <= 0) {
        error = "IPC timed out";
        return false;
      }
      pollfd pfd{fd_, events, 0};
      const int pr = poll(&pfd, 1, static_cast<int>(remaining));
      if (pr < 0 && errno == EINTR) continue;
      if (pr > 0) return true;
    }
  }

  int fd_ = -1;
};

}  // namespace

std::string HyprlandSocketPath() {
  std::vector<std::string> bases;
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime != nullptr && *runtime != '\0') bases.push_back(std::string(runtime) + "/hypr");
  bases.push_back("/tmp/hypr");
  const char* sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (sig != nullptr && *sig != '\0') {
    for (const std::string& base : bases) {
      const std::string p = base + "/" + sig + "/.socket.sock";
      if (IsSocket(p)) return p;
    }
    return std::string();
  }
  // No signature in the environment: newest instance directory with a socket.
  std::string best;
  time_t best_mtime = 0;
  for (const std::string& base : bases) {
    DIR* dir = opendir(base.c_str());
    if (dir == nullptr) continue;
    while (dirent* e = readdir(dir)) {
      if (e->d_name[0] == '.') continue;
      const std::string p = base + "/" + e->d_name + "/.socket.sock";
      struct stat st {};
      if (stat(p.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && (best.empty() || st.st_mtime > best_mtime)) {
        best = p;
        best_mtime = st.st_mtime;
      }
    }
    closedir(dir);
  }
  return best;
}

bool HyprlandQuery(const std::string& command, Json& out, std::string& error) {
  const std::string path = HyprlandSocketPath();
  if (path.empty()) {
    error = "Hyprland IPC socket not found";
    return false;
  }
  const int64_t deadline = NowMs() + kIpcTimeoutMs;
  Socket s;
  std::string reply;
  if (!s.Connect(path, error) || !s.WriteAll(command.data(), command.size(), deadline, error) ||
      !s.Read(reply, 0, true, deadline, error)) {
    return false;
  }
  if (!Json::Parse(reply, out, error)) {
    error = "Hyprland " + command + " reply is not JSON: " + error;
    return false;
  }
  return true;
}

std::string SwaySocketPath() {
  const char* sock = std::getenv("SWAYSOCK");
  if (sock != nullptr && *sock != '\0' && IsSocket(sock)) return sock;
  return std::string();
}

bool SwayQuery(uint32_t type, Json& out, std::string& error) {
  const std::string path = SwaySocketPath();
  if (path.empty()) {
    error = "sway IPC socket not found";
    return false;
  }
  static const char kMagic[] = {'i', '3', '-', 'i', 'p', 'c'};
  const int64_t deadline = NowMs() + kIpcTimeoutMs;
  Socket s;
  if (!s.Connect(path, error)) return false;
  char header[14];
  std::memcpy(header, kMagic, 6);
  const uint32_t zero = 0;
  std::memcpy(header + 6, &zero, 4);
  std::memcpy(header + 10, &type, 4);
  std::string reply_header;
  if (!s.WriteAll(header, sizeof(header), deadline, error) || !s.Read(reply_header, 14, false, deadline, error)) {
    return false;
  }
  if (std::memcmp(reply_header.data(), kMagic, 6) != 0) {
    error = "invalid sway IPC reply";
    return false;
  }
  uint32_t length = 0;
  std::memcpy(&length, reply_header.data() + 6, 4);
  if (length > kMaxReplyBytes) {
    error = "sway IPC reply too large";
    return false;
  }
  std::string payload;
  if (!s.Read(payload, length, false, deadline, error)) return false;
  if (!Json::Parse(payload, out, error)) {
    error = "sway IPC reply is not JSON: " + error;
    return false;
  }
  return true;
}

}  // namespace screen
}  // namespace plaud_rec
