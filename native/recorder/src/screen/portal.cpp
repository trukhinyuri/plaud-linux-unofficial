// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "portal.h"

#include <dbus/dbus.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

namespace plaud_rec {
namespace screen {

namespace {

constexpr const char* kPortalBus = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenshotIface = "org.freedesktop.portal.Screenshot";
constexpr const char* kRequestIface = "org.freedesktop.portal.Request";

int64_t NowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

class ScopedError {
 public:
  ScopedError() { dbus_error_init(&e_); }
  ~ScopedError() { dbus_error_free(&e_); }
  DBusError* get() { return &e_; }
  bool IsSet() const { return dbus_error_is_set(&e_) != 0; }
  std::string Message() const { return std::string(e_.name ? e_.name : "") + ": " + (e_.message ? e_.message : ""); }

 private:
  DBusError e_;
};

// Private connection: never share (or close) the connection Electron's
// own components may hold on the session bus.
class Connection {
 public:
  bool Open(std::string& error) {
    ScopedError err;
    conn_ = dbus_bus_get_private(DBUS_BUS_SESSION, err.get());
    if (conn_ == nullptr) {
      error = "cannot connect to the session bus: " + err.Message();
      return false;
    }
    dbus_connection_set_exit_on_disconnect(conn_, FALSE);
    return true;
  }
  ~Connection() {
    if (conn_ != nullptr) {
      dbus_connection_close(conn_);
      dbus_connection_unref(conn_);
    }
  }
  DBusConnection* get() const { return conn_; }

 private:
  DBusConnection* conn_ = nullptr;
};

class Message {
 public:
  explicit Message(DBusMessage* m = nullptr) : m_(m) {}
  ~Message() {
    if (m_ != nullptr) dbus_message_unref(m_);
  }
  Message(const Message&) = delete;
  Message& operator=(const Message&) = delete;
  DBusMessage* get() const { return m_; }

 private:
  DBusMessage* m_;
};

bool AppendDictEntry(DBusMessageIter* dict, const char* key, int type, const void* value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  const char sig[2] = {static_cast<char>(type), '\0'};
  return dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry) &&
         dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key) &&
         dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant) &&
         dbus_message_iter_append_basic(&variant, type, value) &&
         dbus_message_iter_close_container(&entry, &variant) && dbus_message_iter_close_container(dict, &entry);
}

std::string MatchRule(const std::string& path) {
  return "type='signal',sender='" + std::string(kPortalBus) + "',interface='" + kRequestIface +
         "',member='Response',path='" + path + "'";
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool FileUriToPath(const std::string& uri, std::string& path) {
  const std::string scheme = "file://";
  if (uri.compare(0, scheme.size(), scheme) != 0) return false;
  std::string rest = uri.substr(scheme.size());
  if (rest.compare(0, 9, "localhost") == 0) rest = rest.substr(9);
  if (rest.empty() || rest[0] != '/') return false;
  path.clear();
  for (size_t i = 0; i < rest.size(); i++) {
    if (rest[i] == '%' && i + 2 < rest.size() && HexValue(rest[i + 1]) >= 0 && HexValue(rest[i + 2]) >= 0) {
      path += static_cast<char>(HexValue(rest[i + 1]) * 16 + HexValue(rest[i + 2]));
      i += 2;
    } else {
      path += rest[i];
    }
  }
  return true;
}

// Parses Response(u response, a{sv} results); extracts results["uri"].
bool ParseResponse(DBusMessage* m, uint32_t& code, std::string& uri) {
  DBusMessageIter it;
  if (!dbus_message_iter_init(m, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
  dbus_uint32_t c = 0;
  dbus_message_iter_get_basic(&it, &c);
  code = c;
  if (!dbus_message_iter_next(&it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return true;
  DBusMessageIter dict;
  dbus_message_iter_recurse(&it, &dict);
  while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(&dict, &entry);
    const char* key = nullptr;
    if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
      dbus_message_iter_get_basic(&entry, &key);
      if (dbus_message_iter_next(&entry) && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT &&
          key != nullptr && std::strcmp(key, "uri") == 0) {
        DBusMessageIter variant;
        dbus_message_iter_recurse(&entry, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
          const char* v = nullptr;
          dbus_message_iter_get_basic(&variant, &v);
          if (v != nullptr) uri = v;
        }
      }
    }
    dbus_message_iter_next(&dict);
  }
  return true;
}

void CloseRequest(DBusConnection* conn, const std::string& handle) {
  Message close(dbus_message_new_method_call(kPortalBus, handle.c_str(), kRequestIface, "Close"));
  if (close.get() == nullptr) return;
  dbus_message_set_no_reply(close.get(), TRUE);
  dbus_connection_send(conn, close.get(), nullptr);
  dbus_connection_flush(conn);
}

}  // namespace

bool PortalScreenshotVersion(uint32_t& version, std::string& error) {
  Connection conn;
  if (!conn.Open(error)) return false;
  Message call(dbus_message_new_method_call(kPortalBus, kPortalPath, "org.freedesktop.DBus.Properties", "Get"));
  const char* iface = kScreenshotIface;
  const char* prop = "version";
  if (call.get() == nullptr ||
      !dbus_message_append_args(call.get(), DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID)) {
    error = "out of memory building D-Bus message";
    return false;
  }
  ScopedError err;
  Message reply(dbus_connection_send_with_reply_and_block(conn.get(), call.get(), 5000, err.get()));
  if (reply.get() == nullptr) {
    error = "portal Screenshot interface unavailable: " + err.Message();
    return false;
  }
  DBusMessageIter it;
  DBusMessageIter variant;
  if (!dbus_message_iter_init(reply.get(), &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) {
    error = "unexpected reply to portal version query";
    return false;
  }
  dbus_message_iter_recurse(&it, &variant);
  if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_UINT32) {
    error = "unexpected portal version type";
    return false;
  }
  dbus_uint32_t v = 0;
  dbus_message_iter_get_basic(&variant, &v);
  version = v;
  return true;
}

bool PortalScreenshot(std::string& file_path, std::string& error, int timeout_ms) {
  static std::atomic<unsigned> counter{0};
  Connection conn;
  if (!conn.Open(error)) return false;
  const char* unique = dbus_bus_get_unique_name(conn.get());
  if (unique == nullptr || unique[0] != ':') {
    error = "session bus did not assign a unique name";
    return false;
  }
  std::string sender(unique + 1);
  for (char& c : sender) {
    if (c == '.') c = '_';
  }
  const std::string token = "plaud_rec_" + std::to_string(getpid()) + "_" + std::to_string(++counter);
  std::string handle = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
  {
    ScopedError err;
    dbus_bus_add_match(conn.get(), MatchRule(handle).c_str(), err.get());
    if (err.IsSet()) {
      error = "cannot subscribe to portal responses: " + err.Message();
      return false;
    }
  }

  Message call(dbus_message_new_method_call(kPortalBus, kPortalPath, kScreenshotIface, "Screenshot"));
  if (call.get() == nullptr) {
    error = "out of memory building D-Bus message";
    return false;
  }
  DBusMessageIter args;
  DBusMessageIter dict;
  dbus_message_iter_init_append(call.get(), &args);
  const char* parent_window = "";
  const char* token_c = token.c_str();
  const dbus_bool_t no = FALSE;
  if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent_window) ||
      !dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict) ||
      !AppendDictEntry(&dict, "handle_token", DBUS_TYPE_STRING, &token_c) ||
      !AppendDictEntry(&dict, "modal", DBUS_TYPE_BOOLEAN, &no) ||
      !AppendDictEntry(&dict, "interactive", DBUS_TYPE_BOOLEAN, &no) ||
      !dbus_message_iter_close_container(&args, &dict)) {
    error = "out of memory building D-Bus message";
    return false;
  }
  const int64_t deadline = NowMs() + timeout_ms;
  {
    ScopedError err;
    Message reply(dbus_connection_send_with_reply_and_block(conn.get(), call.get(), timeout_ms, err.get()));
    if (reply.get() == nullptr) {
      error = "portal Screenshot call failed: " + err.Message();
      return false;
    }
    const char* returned = nullptr;
    ScopedError parse;
    if (!dbus_message_get_args(reply.get(), parse.get(), DBUS_TYPE_OBJECT_PATH, &returned, DBUS_TYPE_INVALID) ||
        returned == nullptr) {
      error = "unexpected portal Screenshot reply: " + parse.Message();
      return false;
    }
    if (handle != returned) {  // pre-0.9 portals ignore handle_token
      handle = returned;
      ScopedError match;
      dbus_bus_add_match(conn.get(), MatchRule(handle).c_str(), match.get());
    }
  }

  for (;;) {
    while (DBusMessage* raw = dbus_connection_pop_message(conn.get())) {
      Message m(raw);
      const char* path = dbus_message_get_path(raw);
      if (!dbus_message_is_signal(raw, kRequestIface, "Response") || path == nullptr || handle != path) continue;
      uint32_t code = 2;
      std::string uri;
      if (!ParseResponse(raw, code, uri)) {
        error = "malformed portal Response signal";
        return false;
      }
      if (code != 0) {
        error = code == 1 ? "portal screenshot was cancelled" : "portal screenshot failed (response " +
                                                                    std::to_string(code) + ")";
        return false;
      }
      if (!FileUriToPath(uri, file_path)) {
        error = "portal returned an unsupported screenshot URI: " + uri;
        return false;
      }
      return true;
    }
    const int64_t remaining = deadline - NowMs();
    if (remaining <= 0) {
      CloseRequest(conn.get(), handle);
      error = "timed out waiting for the portal screenshot (" + std::to_string(timeout_ms) + " ms)";
      return false;
    }
    if (!dbus_connection_read_write(conn.get(), static_cast<int>(remaining))) {
      error = "session bus connection lost while waiting for the portal";
      return false;
    }
  }
}

}  // namespace screen
}  // namespace plaud_rec
