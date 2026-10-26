// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "wayland_outputs.h"

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
#include <map>

namespace plaud_rec {
namespace screen {

namespace {

int64_t NowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool IsSocket(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
}

class Message {
 public:
  Message(uint32_t object_id, uint16_t opcode) : words_{object_id, opcode} {}
  void Uint(uint32_t v) { words_.push_back(v); }
  void String(const std::string& s) {
    const uint32_t len = static_cast<uint32_t>(s.size() + 1);
    words_.push_back(len);
    const size_t start = words_.size();
    words_.resize(start + (len + 3) / 4, 0);
    std::memcpy(&words_[start], s.c_str(), len);
  }
  const std::vector<uint32_t>& Finish() {
    words_[1] = (static_cast<uint32_t>(words_.size() * 4) << 16) | (words_[1] & 0xffffu);
    return words_;
  }

 private:
  std::vector<uint32_t> words_;
};

class ArgReader {
 public:
  ArgReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool Uint(uint32_t& v) {
    if (size_ - pos_ < 4) return false;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return true;
  }
  bool Int(int32_t& v) {
    uint32_t u = 0;
    if (!Uint(u)) return false;
    std::memcpy(&v, &u, 4);
    return true;
  }
  bool String(std::string& s) {
    uint32_t len = 0;
    if (!Uint(len)) return false;
    if (len == 0) {
      s.clear();
      return true;
    }
    const size_t padded = (static_cast<size_t>(len) + 3) & ~static_cast<size_t>(3);
    if (size_ - pos_ < padded) return false;
    s.assign(reinterpret_cast<const char*>(data_ + pos_), len - 1);
    pos_ += padded;
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

struct Global {
  uint32_t name;
  std::string interface;
  uint32_t version;
};

class WireClient {
 public:
  ~WireClient() {
    if (fd_ >= 0) close(fd_);
  }

  bool Connect(const std::string& path, std::string& error) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      error = "Wayland socket path too long";
      return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
      error = std::string("socket: ") + std::strerror(errno);
      return false;
    }
    if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      error = "connect(" + path + "): " + std::strerror(errno);
      return false;
    }
    return true;
  }

  bool Send(Message& m, std::string& error) {
    const std::vector<uint32_t>& w = m.Finish();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(w.data());
    size_t left = w.size() * 4;
    while (left > 0) {
      const ssize_t k = send(fd_, p, left, MSG_NOSIGNAL);
      if (k < 0 && errno == EINTR) continue;
      if (k <= 0) {
        error = std::string("send: ") + std::strerror(errno);
        return false;
      }
      p += k;
      left -= static_cast<size_t>(k);
    }
    return true;
  }

  // Reads and dispatches events until `done()` returns true.
  template <typename Dispatch, typename Done>
  bool Pump(int64_t deadline, Dispatch dispatch, Done done, std::string& error) {
    uint8_t chunk[8192];
    while (!done()) {
      while (buffer_.size() >= 8) {
        uint32_t object = 0;
        uint32_t size_opcode = 0;
        std::memcpy(&object, buffer_.data(), 4);
        std::memcpy(&size_opcode, buffer_.data() + 4, 4);
        const uint32_t size = size_opcode >> 16;
        if (size < 8 || (size & 3) != 0) {
          error = "malformed Wayland message";
          return false;
        }
        if (buffer_.size() < size) break;
        if (!dispatch(object, static_cast<uint16_t>(size_opcode & 0xffffu), buffer_.data() + 8, size - 8, error)) {
          return false;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + size);
        if (done()) return true;
      }
      const int64_t remaining = deadline - NowMs();
      if (remaining <= 0) {
        error = "timed out waiting for the Wayland compositor";
        return false;
      }
      pollfd pfd{fd_, POLLIN, 0};
      const int pr = poll(&pfd, 1, static_cast<int>(remaining));
      if (pr < 0 && errno == EINTR) continue;
      if (pr <= 0) continue;
      const ssize_t k = recv(fd_, chunk, sizeof(chunk), MSG_CMSG_CLOEXEC);
      if (k < 0 && errno == EINTR) continue;
      if (k <= 0) {
        error = "Wayland compositor closed the connection";
        return false;
      }
      buffer_.insert(buffer_.end(), chunk, chunk + k);
    }
    return true;
  }

 private:
  int fd_ = -1;
  std::vector<uint8_t> buffer_;
};

}  // namespace

bool ResolveWaylandSocket(std::string& socket_path, std::string& display_name) {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  const char* display = std::getenv("WAYLAND_DISPLAY");
  if (display != nullptr && *display != '\0') {
    display_name = display;
    if (display[0] == '/') {
      socket_path = display;
    } else if (runtime != nullptr && *runtime != '\0') {
      socket_path = std::string(runtime) + "/" + display;
    } else {
      return false;
    }
    return IsSocket(socket_path);
  }
  if (runtime == nullptr || *runtime == '\0') return false;
  DIR* dir = opendir(runtime);
  if (dir == nullptr) return false;
  std::vector<std::string> names;
  while (dirent* e = readdir(dir)) {
    const std::string n = e->d_name;
    if (n.compare(0, 8, "wayland-") == 0 && n.find('.') == std::string::npos) names.push_back(n);
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    const std::string p = std::string(runtime) + "/" + n;
    if (IsSocket(p)) {
      socket_path = p;
      display_name = n;
      return true;
    }
  }
  return false;
}

bool QueryWaylandOutputs(WaylandInfo& out, std::string& error, int timeout_ms) {
  out = WaylandInfo();
  std::string socket_path;
  std::string display_name;
  if (!ResolveWaylandSocket(socket_path, display_name)) {
    error = "no Wayland compositor socket found";
    return false;
  }
  WireClient wire;
  if (!wire.Connect(socket_path, error)) return false;
  const int64_t deadline = NowMs() + timeout_ms;

  constexpr uint32_t kDisplay = 1;
  constexpr uint32_t kRegistry = 2;
  uint32_t next_id = 3;
  std::vector<Global> globals;
  std::map<uint32_t, bool> callbacks_done;
  std::map<uint32_t, size_t> output_ids;      // wl_output object id -> index in out.outputs
  std::map<uint32_t, size_t> xdg_output_ids;  // zxdg_output_v1 object id -> index

  auto dispatch = [&](uint32_t object, uint16_t opcode, const uint8_t* data, size_t size, std::string& err) {
    ArgReader args(data, size);
    if (object == kDisplay) {
      if (opcode == 0) {
        uint32_t obj = 0;
        uint32_t code = 0;
        std::string message;
        args.Uint(obj);
        args.Uint(code);
        args.String(message);
        err = "Wayland protocol error " + std::to_string(code) + " on object " + std::to_string(obj) + ": " + message;
        return false;
      }
      return true;  // delete_id
    }
    if (object == kRegistry) {
      if (opcode == 0) {
        Global g{0, std::string(), 0};
        if (!args.Uint(g.name) || !args.String(g.interface) || !args.Uint(g.version)) {
          err = "malformed wl_registry.global";
          return false;
        }
        globals.push_back(g);
      } else if (opcode == 1) {
        uint32_t name = 0;
        args.Uint(name);
        globals.erase(std::remove_if(globals.begin(), globals.end(), [name](const Global& g) { return g.name == name; }),
                      globals.end());
      }
      return true;
    }
    auto cb = callbacks_done.find(object);
    if (cb != callbacks_done.end()) {
      if (opcode == 0) cb->second = true;
      return true;
    }
    auto o = output_ids.find(object);
    if (o != output_ids.end()) {
      WaylandOutput& w = out.outputs[o->second];
      switch (opcode) {
        case 0: {  // geometry
          int32_t pw = 0;
          int32_t ph = 0;
          int32_t subpixel = 0;
          std::string make;
          std::string model;
          args.Int(w.geometry_x);
          args.Int(w.geometry_y);
          args.Int(pw);
          args.Int(ph);
          args.Int(subpixel);
          args.String(make);
          args.String(model);
          args.Int(w.transform);
          break;
        }
        case 1: {  // mode
          uint32_t flags = 0;
          int32_t mw = 0;
          int32_t mh = 0;
          args.Uint(flags);
          args.Int(mw);
          args.Int(mh);
          if ((flags & 1u) != 0) {
            w.mode_width = mw;
            w.mode_height = mh;
          }
          break;
        }
        case 3:  // scale
          args.Int(w.scale);
          break;
        case 4:  // name
          args.String(w.name);
          break;
        case 5:  // description
          args.String(w.description);
          break;
        default:
          break;
      }
      return true;
    }
    auto x = xdg_output_ids.find(object);
    if (x != xdg_output_ids.end()) {
      WaylandOutput& w = out.outputs[x->second];
      switch (opcode) {
        case 0:
          args.Int(w.logical_x);
          args.Int(w.logical_y);
          w.has_logical = true;
          break;
        case 1:
          args.Int(w.logical_width);
          args.Int(w.logical_height);
          w.has_logical = true;
          break;
        case 3: {
          std::string name;
          args.String(name);
          if (w.name.empty()) w.name = name;
          break;
        }
        default:
          break;
      }
      return true;
    }
    return true;
  };

  auto roundtrip = [&](std::string& err) {
    const uint32_t cb = next_id++;
    callbacks_done[cb] = false;
    Message sync(kDisplay, 0);
    sync.Uint(cb);
    if (!wire.Send(sync, err)) return false;
    return wire.Pump(deadline, dispatch, [&]() { return callbacks_done[cb]; }, err);
  };

  Message get_registry(kDisplay, 1);
  get_registry.Uint(kRegistry);
  if (!wire.Send(get_registry, error) || !roundtrip(error)) return false;

  uint32_t xdg_manager = 0;
  for (const Global& g : globals) {
    if (g.interface == "wl_output") {
      const uint32_t id = next_id++;
      Message bind(kRegistry, 0);
      bind.Uint(g.name);
      bind.String("wl_output");
      bind.Uint(std::min<uint32_t>(g.version, 4));
      bind.Uint(id);
      if (!wire.Send(bind, error)) return false;
      WaylandOutput o;
      o.global_name = g.name;
      out.outputs.push_back(o);
      output_ids[id] = out.outputs.size() - 1;
    } else if (g.interface == "zxdg_output_manager_v1" && xdg_manager == 0) {
      xdg_manager = next_id++;
      Message bind(kRegistry, 0);
      bind.Uint(g.name);
      bind.String("zxdg_output_manager_v1");
      bind.Uint(std::min<uint32_t>(g.version, 3));
      bind.Uint(xdg_manager);
      if (!wire.Send(bind, error)) return false;
      out.has_xdg_output_manager = true;
    } else if (g.interface == "zwlr_screencopy_manager_v1") {
      out.has_wlr_screencopy = true;
    } else if (g.interface == "ext_image_copy_capture_manager_v1") {
      out.has_ext_image_copy = true;
    }
  }
  if (xdg_manager != 0) {
    for (const auto& kv : output_ids) {
      const uint32_t id = next_id++;
      Message get(xdg_manager, 1);
      get.Uint(id);
      get.Uint(kv.first);
      if (!wire.Send(get, error)) return false;
      xdg_output_ids[id] = kv.second;
    }
  }
  if (!roundtrip(error)) return false;
  return true;
}

}  // namespace screen
}  // namespace plaud_rec
