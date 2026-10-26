// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "window_titles.h"

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "json.h"
#include "proc_util.h"

namespace am {

namespace {

constexpr int kIpcTimeoutMs = 400;

int connectUnix(const std::string& path) {
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  timeval tv{};
  tv.tv_sec = kIpcTimeoutMs / 1000;
  tv.tv_usec = (kIpcTimeoutMs % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool writeAll(int fd, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  while (len > 0) {
    ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool readExact(int fd, void* data, size_t len) {
  char* p = static_cast<char*>(data);
  while (len > 0) {
    ssize_t n = ::recv(fd, p, len, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

std::string readToEof(int fd, size_t limit) {
  std::string out;
  char buf[8192];
  while (out.size() < limit) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

// ---------------------------------------------------------------------------
class FileProvider : public WindowTitleProvider {
 public:
  explicit FileProvider(std::string path) : path_(std::move(path)) {}
  std::string name() const override { return "file"; }
  bool list(std::vector<WindowInfo>& out) override {
    std::string text = readSmallFile(path_, 4 * 1024 * 1024);
    JsonValue root;
    if (text.empty() || !parseJson(text, root) || !root.isArray()) return false;
    for (const JsonValue& item : root.items()) {
      WindowInfo w;
      w.pid = static_cast<int>(item.intOr("pid", 0));
      w.appId = item.stringOr("appId", item.stringOr("class"));
      w.title = item.stringOr("title");
      out.push_back(std::move(w));
    }
    return true;
  }

 private:
  std::string path_;
};

// ---------------------------------------------------------------------------
class HyprlandProvider : public WindowTitleProvider {
 public:
  explicit HyprlandProvider(std::vector<std::string> sockets) : sockets_(std::move(sockets)) {}
  std::string name() const override { return "hyprland"; }
  bool list(std::vector<WindowInfo>& out) override {
    for (const std::string& path : sockets_) {
      int fd = connectUnix(path);
      if (fd < 0) continue;
      static const char kRequest[] = "j/clients";
      std::string reply;
      if (writeAll(fd, kRequest, sizeof(kRequest) - 1)) reply = readToEof(fd, 16 * 1024 * 1024);
      ::close(fd);
      JsonValue root;
      if (reply.empty() || !parseJson(reply, root) || !root.isArray()) continue;
      for (const JsonValue& c : root.items()) {
        WindowInfo w;
        w.pid = static_cast<int>(c.intOr("pid", 0));
        w.appId = c.stringOr("class", c.stringOr("initialClass"));
        w.title = c.stringOr("title");
        out.push_back(std::move(w));
      }
      return true;
    }
    return false;
  }

 private:
  std::vector<std::string> sockets_;
};

// ---------------------------------------------------------------------------
class SwayProvider : public WindowTitleProvider {
 public:
  explicit SwayProvider(std::string socket) : socket_(std::move(socket)) {}
  std::string name() const override { return "sway-ipc"; }
  bool list(std::vector<WindowInfo>& out) override {
    int fd = connectUnix(socket_);
    if (fd < 0) return false;
    // i3 IPC: "i3-ipc" + uint32 length + uint32 type (native byte order).
    char header[14];
    std::memcpy(header, "i3-ipc", 6);
    uint32_t length = 0;
    uint32_t type = 4;  // GET_TREE
    std::memcpy(header + 6, &length, 4);
    std::memcpy(header + 10, &type, 4);
    std::string payload;
    char replyHeader[14];
    if (writeAll(fd, header, sizeof(header)) && readExact(fd, replyHeader, sizeof(replyHeader)) &&
        std::memcmp(replyHeader, "i3-ipc", 6) == 0) {
      uint32_t replyLen = 0;
      std::memcpy(&replyLen, replyHeader + 6, 4);
      if (replyLen < 64 * 1024 * 1024) {
        payload.resize(replyLen);
        if (!readExact(fd, &payload[0], replyLen)) payload.clear();
      }
    }
    ::close(fd);
    JsonValue root;
    if (payload.empty() || !parseJson(payload, root)) return false;
    walk(root, out, 0);
    return true;
  }

 private:
  static void walk(const JsonValue& node, std::vector<WindowInfo>& out, int depth) {
    if (!node.isObject() || depth > 64) return;
    const JsonValue& pid = node["pid"];
    const std::string type = node.stringOr("type");
    if (pid.isNumber() && (type == "con" || type == "floating_con")) {
      WindowInfo w;
      w.pid = static_cast<int>(pid.asNumber());
      w.title = node.stringOr("name");
      w.appId = node.stringOr("app_id", node["window_properties"].stringOr("class"));
      out.push_back(std::move(w));
    }
    for (const char* key : {"nodes", "floating_nodes"}) {
      for (const JsonValue& child : node[key].items()) walk(child, out, depth + 1);
    }
  }

  std::string socket_;
};

// ---------------------------------------------------------------------------
// Minimal libxcb ABI (stable X11 protocol structures), resolved with dlopen.
struct xcb_connection_t;
struct xcb_setup_t;
struct xcb_generic_error_t;
using xcb_window_t = uint32_t;
using xcb_atom_t = uint32_t;
struct xcb_screen_t {
  xcb_window_t root;  // first member of xcb_screen_t
};
struct xcb_screen_iterator_t {
  xcb_screen_t* data;
  int rem;
  int index;
};
struct xcb_intern_atom_cookie_t {
  unsigned int sequence;
};
struct xcb_intern_atom_reply_t {
  uint8_t response_type;
  uint8_t pad0;
  uint16_t sequence;
  uint32_t length;
  xcb_atom_t atom;
};
struct xcb_get_property_cookie_t {
  unsigned int sequence;
};
struct xcb_get_property_reply_t {
  uint8_t response_type;
  uint8_t format;
  uint16_t sequence;
  uint32_t length;
  xcb_atom_t type;
  uint32_t bytes_after;
  uint32_t value_len;
  uint8_t pad0[12];
};

constexpr xcb_atom_t kAtomCardinal = 6;
constexpr xcb_atom_t kAtomString = 31;
constexpr xcb_atom_t kAtomWindow = 33;
constexpr xcb_atom_t kAtomWmName = 39;
constexpr xcb_atom_t kAtomWmClass = 67;

class X11Provider : public WindowTitleProvider {
 public:
  ~X11Provider() override {
    if (conn_ != nullptr) disconnect_(conn_);
    if (lib_ != nullptr) ::dlclose(lib_);
  }

  static std::unique_ptr<X11Provider> create() {
    const char* display = std::getenv("DISPLAY");
    if (display == nullptr || *display == '\0') return nullptr;
    std::unique_ptr<X11Provider> p(new X11Provider());
    p->lib_ = ::dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
    if (p->lib_ == nullptr) return nullptr;
#define AM_XCB_SYM(field, symbol)                                                     \
  p->field = reinterpret_cast<decltype(p->field)>(::dlsym(p->lib_, symbol)); \
  if (p->field == nullptr) return nullptr;
    AM_XCB_SYM(connect_, "xcb_connect")
    AM_XCB_SYM(hasError_, "xcb_connection_has_error")
    AM_XCB_SYM(disconnect_, "xcb_disconnect")
    AM_XCB_SYM(getSetup_, "xcb_get_setup")
    AM_XCB_SYM(rootsIterator_, "xcb_setup_roots_iterator")
    AM_XCB_SYM(screenNext_, "xcb_screen_next")
    AM_XCB_SYM(internAtom_, "xcb_intern_atom")
    AM_XCB_SYM(internAtomReply_, "xcb_intern_atom_reply")
    AM_XCB_SYM(getProperty_, "xcb_get_property")
    AM_XCB_SYM(getPropertyReply_, "xcb_get_property_reply")
    AM_XCB_SYM(propertyValue_, "xcb_get_property_value")
    AM_XCB_SYM(propertyValueLength_, "xcb_get_property_value_length")
#undef AM_XCB_SYM
    return p;
  }

  std::string name() const override { return "x11"; }

  bool list(std::vector<WindowInfo>& out) override {
    if (!ensureConnected()) return false;
    std::vector<uint32_t> clients;
    if (!getCardinals(root_, atomClientList_, kAtomWindow, clients)) {
      dropConnection();
      return false;
    }
    for (uint32_t window : clients) {
      WindowInfo w;
      std::vector<uint32_t> pid;
      if (getCardinals(window, atomWmPid_, kAtomCardinal, pid) && !pid.empty()) w.pid = static_cast<int>(pid[0]);
      w.title = getText(window, atomNetWmName_, atomUtf8_);
      if (w.title.empty()) w.title = getText(window, kAtomWmName, kAtomString);
      std::string wmClass = getText(window, kAtomWmClass, kAtomString);
      // WM_CLASS = "instance\0class\0"
      size_t nul = wmClass.find('\0');
      w.appId = nul == std::string::npos ? wmClass : wmClass.substr(nul + 1);
      while (!w.appId.empty() && w.appId.back() == '\0') w.appId.pop_back();
      out.push_back(std::move(w));
    }
    return true;
  }

 private:
  X11Provider() = default;

  bool ensureConnected() {
    if (conn_ != nullptr && hasError_(conn_) == 0) return true;
    dropConnection();
    int screen = 0;
    conn_ = connect_(nullptr, &screen);
    if (conn_ == nullptr || hasError_(conn_) != 0) {
      dropConnection();
      return false;
    }
    xcb_screen_iterator_t it = rootsIterator_(getSetup_(conn_));
    for (int i = 0; i < screen && it.rem > 1; ++i) screenNext_(&it);
    if (it.data == nullptr) {
      dropConnection();
      return false;
    }
    root_ = it.data->root;
    atomClientList_ = intern("_NET_CLIENT_LIST");
    atomWmPid_ = intern("_NET_WM_PID");
    atomNetWmName_ = intern("_NET_WM_NAME");
    atomUtf8_ = intern("UTF8_STRING");
    if (atomClientList_ == 0) {
      dropConnection();
      return false;
    }
    return true;
  }

  void dropConnection() {
    if (conn_ != nullptr) disconnect_(conn_);
    conn_ = nullptr;
  }

  xcb_atom_t intern(const char* atomName) {
    xcb_intern_atom_cookie_t cookie =
        internAtom_(conn_, 1, static_cast<uint16_t>(std::strlen(atomName)), atomName);
    xcb_intern_atom_reply_t* reply = internAtomReply_(conn_, cookie, nullptr);
    if (reply == nullptr) return 0;
    xcb_atom_t atom = reply->atom;
    std::free(reply);
    return atom;
  }

  bool getCardinals(xcb_window_t window, xcb_atom_t property, xcb_atom_t type, std::vector<uint32_t>& out) {
    if (property == 0) return false;
    xcb_get_property_cookie_t cookie = getProperty_(conn_, 0, window, property, type, 0, 1 << 16);
    xcb_get_property_reply_t* reply = getPropertyReply_(conn_, cookie, nullptr);
    if (reply == nullptr) return false;
    bool ok = reply->format == 32;
    if (ok) {
      int bytes = propertyValueLength_(reply) * 4;
      const uint32_t* values = static_cast<const uint32_t*>(propertyValue_(reply));
      out.assign(values, values + bytes / 4);
    }
    std::free(reply);
    return ok;
  }

  std::string getText(xcb_window_t window, xcb_atom_t property, xcb_atom_t type) {
    if (property == 0 || type == 0) return std::string();
    xcb_get_property_cookie_t cookie = getProperty_(conn_, 0, window, property, type, 0, 4096);
    xcb_get_property_reply_t* reply = getPropertyReply_(conn_, cookie, nullptr);
    if (reply == nullptr) return std::string();
    std::string text;
    if (reply->format == 8) {
      int len = propertyValueLength_(reply);
      text.assign(static_cast<const char*>(propertyValue_(reply)), static_cast<size_t>(len));
    }
    std::free(reply);
    return text;
  }

  void* lib_ = nullptr;
  xcb_connection_t* conn_ = nullptr;
  xcb_window_t root_ = 0;
  xcb_atom_t atomClientList_ = 0;
  xcb_atom_t atomWmPid_ = 0;
  xcb_atom_t atomNetWmName_ = 0;
  xcb_atom_t atomUtf8_ = 0;

  xcb_connection_t* (*connect_)(const char*, int*) = nullptr;
  int (*hasError_)(xcb_connection_t*) = nullptr;
  void (*disconnect_)(xcb_connection_t*) = nullptr;
  const xcb_setup_t* (*getSetup_)(xcb_connection_t*) = nullptr;
  xcb_screen_iterator_t (*rootsIterator_)(const xcb_setup_t*) = nullptr;
  void (*screenNext_)(xcb_screen_iterator_t*) = nullptr;
  xcb_intern_atom_cookie_t (*internAtom_)(xcb_connection_t*, uint8_t, uint16_t, const char*) = nullptr;
  xcb_intern_atom_reply_t* (*internAtomReply_)(xcb_connection_t*, xcb_intern_atom_cookie_t,
                                                xcb_generic_error_t**) = nullptr;
  xcb_get_property_cookie_t (*getProperty_)(xcb_connection_t*, uint8_t, xcb_window_t, xcb_atom_t, xcb_atom_t,
                                            uint32_t, uint32_t) = nullptr;
  xcb_get_property_reply_t* (*getPropertyReply_)(xcb_connection_t*, xcb_get_property_cookie_t,
                                                  xcb_generic_error_t**) = nullptr;
  void* (*propertyValue_)(const xcb_get_property_reply_t*) = nullptr;
  int (*propertyValueLength_)(const xcb_get_property_reply_t*) = nullptr;
};

}  // namespace

std::unique_ptr<WindowTitleProvider> makeFileWindowProvider(const std::string& path) {
  return std::unique_ptr<WindowTitleProvider>(new FileProvider(path));
}

std::unique_ptr<WindowTitleProvider> makeHyprlandWindowProvider() {
  const char* sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (sig == nullptr || *sig == '\0') return nullptr;
  std::vector<std::string> sockets;
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
    sockets.push_back(std::string(runtime) + "/hypr/" + sig + "/.socket.sock");
  }
  sockets.push_back(std::string("/tmp/hypr/") + sig + "/.socket.sock");  // Hyprland < 0.40
  return std::unique_ptr<WindowTitleProvider>(new HyprlandProvider(std::move(sockets)));
}

std::unique_ptr<WindowTitleProvider> makeSwayWindowProvider() {
  const char* sock = std::getenv("SWAYSOCK");
  if (sock == nullptr || *sock == '\0') sock = std::getenv("I3SOCK");
  if (sock == nullptr || *sock == '\0') return nullptr;
  return std::unique_ptr<WindowTitleProvider>(new SwayProvider(sock));
}

std::unique_ptr<WindowTitleProvider> makeX11WindowProvider() { return X11Provider::create(); }

std::unique_ptr<WindowTitleProvider> makeDefaultWindowProvider() {
  if (const char* file = std::getenv("PLAUD_AUDIO_MONITOR_WINDOW_LIST")) {
    if (*file != '\0') return makeFileWindowProvider(file);
  }
  if (auto p = makeHyprlandWindowProvider()) return p;
  if (auto p = makeSwayWindowProvider()) return p;
  if (auto p = makeX11WindowProvider()) return p;
  return nullptr;
}

std::string meetingServiceFromTitle(const std::string& title) {
  const std::string t = toLower(title);
  struct Rule {
    const char* needle;
    const char* service;
    bool prefix;
  };
  static const Rule kRules[] = {
      {"meet - ", "google-meet", true},
      {"google meet", "google-meet", false},
      {"meet.google.com", "google-meet", false},
      {"microsoft teams", "teams", false},
      {"teams.microsoft.com", "teams", false},
      {"teams.live.com", "teams", false},
      {"zoom meeting", "zoom", false},
      {"zoom webinar", "zoom", false},
      {"zoom workplace", "zoom", false},
      {"app.zoom.us", "zoom", false},
      {"webex", "webex", false},
      {"jitsi meet", "jitsi", false},
      {"meet.jit.si", "jitsi", false},
      {"whereby", "whereby", false},
      {"lark meeting", "lark", false},
      {"lark video", "lark", false},
      {"feishu meeting", "lark", false},
      {"\xE9\xA3\x9E\xE4\xB9\xA6\xE4\xBC\x9A\xE8\xAE\xAE", "lark", false},  // 飞书会议
      {"\xE8\x85\xBE\xE8\xAE\xAF\xE4\xBC\x9A\xE8\xAE\xAE", "tencent", false},  // 腾讯会议
      {"voov meeting", "tencent", false},
      {"goto meeting", "goto", false},
      {"gotomeeting", "goto", false},
      {"ringcentral video", "ringcentral", false},
      {"bluejeans", "bluejeans", false},
  };
  for (const Rule& r : kRules) {
    if (r.prefix ? startsWith(t, r.needle) : t.find(r.needle) != std::string::npos) return r.service;
  }
  if (t.find("huddle") != std::string::npos && t.find("slack") != std::string::npos) return "slack";
  return std::string();
}

}  // namespace am
