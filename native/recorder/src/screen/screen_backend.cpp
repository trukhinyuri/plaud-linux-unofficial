// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "screen_backend.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

#include "compositor_ipc.h"
#include "image.h"
#include "json.h"
#include "portal.h"
#include "spawn.h"
#include "wayland_outputs.h"
#include "x11.h"

namespace plaud_rec {
namespace screen {

namespace {

constexpr int kGrimTimeoutMs = 10000;
constexpr int kPortalTimeoutMs = 15000;
constexpr int kWaylandQueryTimeoutMs = 2000;
constexpr double kMinWindowPixels = 50.0;  // Windows build ignores windows below 50 px
constexpr long long kMatchTolerance = 2;   // px slack when a rect names a whole display
constexpr long long kMaxDimension = 1 << 15;
constexpr size_t kMaxToplevelBytes = 256u << 20;
constexpr size_t kMaxPortalFileBytes = 512u << 20;

enum class Space { X11, Wayland };
enum class Backend { Grim, Portal, X11 };

const char* SpaceName(Space s) { return s == Space::X11 ? "x11" : "wayland"; }

const char* BackendName(Backend b) {
  switch (b) {
    case Backend::Grim:
      return "grim";
    case Backend::Portal:
      return "portal";
    case Backend::X11:
      break;
  }
  return "x11";
}

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::string(v) : std::string();
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Fmt(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  return buf;
}

double JsRound(double v) { return std::floor(v + 0.5); }

struct IRect {
  long long x = 0;
  long long y = 0;
  long long w = 0;
  long long h = 0;
  bool Empty() const { return w <= 0 || h <= 0; }
  std::string ToString() const {
    return "(" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(w) + ", " +
           std::to_string(h) + ")";
  }
};

IRect MakeRect(long long x, long long y, long long w, long long h) {
  IRect r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  return r;
}

IRect Intersect(const IRect& a, const IRect& b) {
  const long long x0 = std::max(a.x, b.x);
  const long long y0 = std::max(a.y, b.y);
  const long long x1 = std::min(a.x + a.w, b.x + b.w);
  const long long y1 = std::min(a.y + a.h, b.y + b.h);
  return MakeRect(x0, y0, std::max(0LL, x1 - x0), std::max(0LL, y1 - y0));
}

bool Near(const IRect& a, const IRect& b) {
  return std::llabs(a.x - b.x) <= kMatchTolerance && std::llabs(a.y - b.y) <= kMatchTolerance &&
         std::llabs(a.w - b.w) <= kMatchTolerance && std::llabs(a.h - b.h) <= kMatchTolerance;
}

struct Display {
  std::string name;
  uint32_t wl_global = 0;  // wl_output registry name (0 when unknown)
  // Compositor layout in logical units (X11 session: root pixels).
  double lx = 0;
  double ly = 0;
  double lw = 0;
  double lh = 0;
  int pixel_w = 0;  // native pixels of the output
  int pixel_h = 0;
  bool has_x11 = false;
  IRect x11;               // X11 root rectangle (XWayland or X11 session)
  uint32_t x11_output = 0;  // RandR output XID = Electron display.id under X11
  bool x11_primary = false;
  int x11_order = -1;

  double Scale() const { return lw > 0 ? pixel_w / lw : 1.0; }
  // Logical position scaled by the output's own scale: the per-output
  // physical space of a Wayland-native client.
  IRect WaylandRect() const {
    const double s = Scale();
    return MakeRect(static_cast<long long>(JsRound(lx * s)), static_cast<long long>(JsRound(ly * s)), pixel_w,
                    pixel_h);
  }
  // physicalBounds as Plaud's renderer computes it when Electron runs
  // natively on Wayland, where screen.dipToScreenPoint is the identity:
  // origin = round(bounds.origin + 2 - 2 * scaleFactor), size = round(bounds.size * scaleFactor).
  IRect ElectronWaylandRect() const {
    const double s = Scale();
    return MakeRect(static_cast<long long>(JsRound(lx + 2 - 2 * s)), static_cast<long long>(JsRound(ly + 2 - 2 * s)),
                    static_cast<long long>(JsRound(lw * s)), static_cast<long long>(JsRound(lh * s)));
  }
  bool ContainsLogical(double x, double y) const { return x >= lx && y >= ly && x < lx + lw && y < ly + lh; }
  std::string Describe() const {
    std::string s = (name.empty() ? std::string("<unnamed>") : name) + " " + std::to_string(pixel_w) + "x" +
                    std::to_string(pixel_h) + " logical=(" + Fmt(lx) + ", " + Fmt(ly) + ", " + Fmt(lw) + ", " +
                    Fmt(lh) + ")";
    if (has_x11) s += " x11=" + x11.ToString() + " xid=" + std::to_string(x11_output);
    return s;
  }
};

struct SessionInfo {
  bool wayland = false;
  std::string wayland_display;  // WAYLAND_DISPLAY to export to helpers (empty: inherited value is fine)
  bool hyprland = false;
  bool sway = false;
};

SessionInfo DetectSession() {
  SessionInfo s;
  const std::string type = Lower(Env("XDG_SESSION_TYPE"));
  const bool env_display = !Env("WAYLAND_DISPLAY").empty();
  s.wayland = type == "wayland" || env_display || !Env("HYPRLAND_INSTANCE_SIGNATURE").empty() ||
              !Env("SWAYSOCK").empty();
  std::string socket_path;
  std::string display_name;
  const bool socket_found = ResolveWaylandSocket(socket_path, display_name);
  if (!s.wayland && socket_found && type != "x11") s.wayland = true;
  if (s.wayland && socket_found && !env_display) s.wayland_display = display_name;
  if (s.wayland) {
    s.hyprland = !HyprlandSocketPath().empty();
    s.sway = !s.hyprland && !SwaySocketPath().empty();
  }
  return s;
}

std::vector<std::string> ReadCmdline() {
  std::ifstream f("/proc/self/cmdline", std::ios::binary);
  const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<std::string> args;
  size_t start = 0;
  for (size_t i = 0; i <= all.size(); i++) {
    if (i == all.size() || all[i] == '\0') {
      if (i > start) args.push_back(all.substr(start, i - start));
      start = i + 1;
    }
  }
  return args;
}

// Chromium semantics: the last occurrence of --name=value wins.
bool LastFlagValue(const std::vector<std::string>& args, const std::string& name, std::string& value) {
  const std::string prefix = "--" + name + "=";
  bool found = false;
  for (const std::string& a : args) {
    if (a.compare(0, prefix.size(), prefix) == 0) {
      value = a.substr(prefix.size());
      found = true;
    }
  }
  return found;
}

// Coordinate space Electron uses for physicalBounds in this process.
Space DecideSpace(const HostInfo& host, std::string& reason) {
  const std::string forced = Lower(Env("PLAUD_SCREEN_COORDS"));
  if (forced == "x11" || forced == "wayland") {
    reason = "PLAUD_SCREEN_COORDS=" + forced;
    return forced == "x11" ? Space::X11 : Space::Wayland;
  }
  const std::vector<std::string> args = ReadCmdline();
  std::string v;
  if (LastFlagValue(args, "ozone-platform", v)) {
    v = Lower(v);
    if (v == "x11" || v == "wayland") {
      reason = "--ozone-platform=" + v;
      return v == "x11" ? Space::X11 : Space::Wayland;
    }
  }
  if (LastFlagValue(args, "ozone-platform-hint", v)) {
    v = Lower(v);
    if (v == "x11" || v == "wayland" || v == "auto") {
      reason = "--ozone-platform-hint=" + v;
      return v == "x11" ? Space::X11 : Space::Wayland;
    }
  }
  if (host.electron_major < 38) {
    const std::string hint = Lower(Env("ELECTRON_OZONE_PLATFORM_HINT"));
    if (hint == "x11" || hint == "wayland" || hint == "auto") {
      reason = "ELECTRON_OZONE_PLATFORM_HINT=" + hint;
      return hint == "x11" ? Space::X11 : Space::Wayland;
    }
  }
  if (host.electron_major >= 38) {
    reason = "Electron " + std::to_string(host.electron_major) + " defaults to native Wayland";
    return Space::Wayland;
  }
  if (!Env("DISPLAY").empty()) {
    reason = "X11 (XWayland) is the Electron default before 38";
    return Space::X11;
  }
  reason = "no X display";
  return Space::Wayland;
}

bool ParseBackend(const std::string& raw, Backend& b) {
  const std::string v = Lower(raw);
  if (v == "grim") {
    b = Backend::Grim;
  } else if (v == "portal") {
    b = Backend::Portal;
  } else if (v == "x11") {
    b = Backend::X11;
  } else {
    return false;
  }
  return true;
}

std::string ProcComm(double pid) {
  if (!(pid > 0)) return std::string();
  std::ifstream f("/proc/" + std::to_string(static_cast<long long>(pid)) + "/comm");
  std::string comm;
  std::getline(f, comm);
  return comm;
}

bool IsTempPath(const std::string& path) {
  std::vector<std::string> roots = {"/tmp/", "/var/tmp/"};
  const std::string runtime = Env("XDG_RUNTIME_DIR");
  if (!runtime.empty()) roots.push_back(runtime + "/");
  const std::string tmpdir = Env("TMPDIR");
  if (!tmpdir.empty()) roots.push_back(tmpdir.back() == '/' ? tmpdir : tmpdir + "/");
  if (path.find("/../") != std::string::npos) return false;
  for (const std::string& r : roots) {
    if (path.compare(0, r.size(), r) == 0) return true;
  }
  return false;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  const std::streamoff size = f.tellg();
  if (size <= 0 || static_cast<unsigned long long>(size) > kMaxPortalFileBytes) {
    error = "unexpected size of " + path;
    return false;
  }
  out.resize(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(out.data()), size)) {
    error = "cannot read " + path;
    return false;
  }
  return true;
}

// A window as the compositor reports it.
struct FoundWindow {
  uint32_t id = 0;
  double x = 0;  // logical layout units (X11 session: root pixels)
  double y = 0;
  double w = 0;
  double h = 0;
  std::string owner;
  std::string title;
  std::string toplevel;  // ext-foreign-toplevel identifier for grim -T (Hyprland stableId)
  bool on_screen = true;
};

struct HyprClient {
  FoundWindow window;
  bool candidate = false;  // mapped, shown, on a visible workspace
  int rank = 0;            // stacking class, higher is above
  double focus = 0;        // focusHistoryID, lower is more recent
};

uint32_t ParseHexId(const std::string& s) {
  if (s.empty()) return 0;
  return static_cast<uint32_t>(std::strtoull(s.c_str(), nullptr, 16) & 0xffffffffULL);
}

bool LoadHyprClients(std::vector<HyprClient>& out, std::string& error) {
  Json monitors;
  Json clients;
  if (!HyprlandQuery("j/monitors", monitors, error) || !HyprlandQuery("j/clients", clients, error)) return false;
  if (!monitors.IsArray() || !clients.IsArray()) {
    error = "unexpected Hyprland IPC reply";
    return false;
  }
  std::set<long long> visible;
  std::set<long long> special;
  for (const Json& m : monitors.Items()) {
    if (const Json* aw = m.Get("activeWorkspace")) visible.insert(static_cast<long long>(aw->NumberAt("id")));
    if (const Json* sw = m.Get("specialWorkspace")) {
      const long long id = static_cast<long long>(sw->NumberAt("id"));
      if (id != 0) {
        visible.insert(id);
        special.insert(id);
      }
    }
  }
  for (const Json& c : clients.Items()) {
    HyprClient h;
    FoundWindow& w = h.window;
    w.toplevel = c.StringAt("stableId");
    w.id = ParseHexId(w.toplevel.empty() ? c.StringAt("address") : w.toplevel);
    const Json* at = c.Get("at");
    const Json* size = c.Get("size");
    if (at == nullptr || size == nullptr || !at->IsArray() || !size->IsArray() || at->Items().size() < 2 ||
        size->Items().size() < 2) {
      continue;
    }
    w.x = at->Items()[0].Number();
    w.y = at->Items()[1].Number();
    w.w = size->Items()[0].Number();
    w.h = size->Items()[1].Number();
    w.owner = c.StringAt("class");
    if (w.owner.empty()) w.owner = c.StringAt("initialClass");
    if (w.owner.empty()) w.owner = ProcComm(c.NumberAt("pid", 0));
    w.title = c.StringAt("title");
    long long workspace = 0;
    if (const Json* ws = c.Get("workspace")) workspace = static_cast<long long>(ws->NumberAt("id"));
    const bool pinned = c.BoolAt("pinned");
    int fullscreen = 0;
    if (const Json* fs = c.Get("fullscreen")) fullscreen = fs->IsNumber() ? static_cast<int>(fs->Number()) : (fs->Bool() ? 2 : 0);
    h.candidate = c.BoolAt("mapped", true) && !c.BoolAt("hidden", false) && c.BoolAt("visible", true) &&
                  (pinned || visible.count(workspace) != 0);
    w.on_screen = h.candidate;
    h.rank = special.count(workspace) != 0 ? 5 : pinned ? 4 : (fullscreen & 2) != 0 ? 3 : c.BoolAt("floating") ? 2 : 1;
    const double fh = c.NumberAt("focusHistoryID", -1);
    h.focus = fh < 0 ? 1e9 : fh;
    out.push_back(h);
  }
  return true;
}

// sway tree walk: first node (depth-first) accepted by `match`.
template <typename Match>
const Json* FindSwayNode(const Json& node, Match match) {
  if (match(node)) return &node;
  for (const char* key : {"nodes", "floating_nodes"}) {
    const Json* children = node.Get(key);
    if (children == nullptr || !children->IsArray()) continue;
    for (const Json& child : children->Items()) {
      if (const Json* found = FindSwayNode(child, match)) return found;
    }
  }
  return nullptr;
}

bool IsSwayWindow(const Json& n) {
  const std::string type = n.StringAt("type");
  return (type == "con" || type == "floating_con") && (n.Get("app_id") != nullptr || n.Get("window_properties") != nullptr) &&
         n.NumberAt("pid", 0) > 0;
}

void FillSwayWindow(const Json& n, FoundWindow& w) {
  w.id = static_cast<uint32_t>(static_cast<unsigned long long>(n.NumberAt("id")) & 0xffffffffULL);
  if (const Json* r = n.Get("rect")) {
    w.x = r->NumberAt("x");
    w.y = r->NumberAt("y");
    w.w = r->NumberAt("width");
    w.h = r->NumberAt("height");
  }
  w.owner = n.StringAt("app_id");
  if (w.owner.empty()) {
    if (const Json* props = n.Get("window_properties")) w.owner = props->StringAt("class");
  }
  if (w.owner.empty()) w.owner = ProcComm(n.NumberAt("pid", 0));
  w.title = n.StringAt("name");
  w.on_screen = n.BoolAt("visible", true);
}

}  // namespace

struct ScreenCapturer::State {
  explicit State(const HostInfo& h) : host(h) {}

  HostInfo host;
  bool initialized = false;
  bool init_ok = false;
  std::string init_error;
  SessionInfo session;
  Space space = Space::X11;
  std::string space_reason;
  std::vector<Display> displays;
  bool screencopy_known = false;
  bool screencopy = false;
  std::unique_ptr<X11Connection> x11;
  bool x11_geometry_tried = false;  // XWayland monitors attached (or attempt failed)
  std::vector<Backend> backends;
  std::string grim_path;

  std::map<size_t, Image> outputs;
  std::map<size_t, std::string> failed_outputs;
  bool portal_tried = false;
  bool portal_ok = false;
  std::string portal_error;
  Image portal_image;

  // ---------------------------------------------------------------- setup

  bool LoadWaylandDisplays(const Log& log) {
    WaylandInfo info;
    std::string error;
    if (QueryWaylandOutputs(info, error, kWaylandQueryTimeoutMs)) {
      screencopy_known = true;
      screencopy = info.has_wlr_screencopy || info.has_ext_image_copy;
      for (const WaylandOutput& o : info.outputs) {
        int pw = o.mode_width;
        int ph = o.mode_height;
        if ((o.transform & 1) != 0) std::swap(pw, ph);
        if (pw <= 0 || ph <= 0) continue;
        Display d;
        d.name = o.name;
        d.wl_global = o.global_name;
        d.pixel_w = pw;
        d.pixel_h = ph;
        if (o.has_logical && o.logical_width > 0 && o.logical_height > 0) {
          d.lx = o.logical_x;
          d.ly = o.logical_y;
          d.lw = o.logical_width;
          d.lh = o.logical_height;
        } else {
          const int s = std::max(1, o.scale);
          d.lx = o.geometry_x;
          d.ly = o.geometry_y;
          d.lw = static_cast<double>(pw) / s;
          d.lh = static_cast<double>(ph) / s;
        }
        displays.push_back(d);
      }
      if (!displays.empty()) return true;
    } else {
      log.Warn("Wayland output query failed: " + error);
    }
    if (session.hyprland) {
      Json monitors;
      if (HyprlandQuery("j/monitors", monitors, error) && monitors.IsArray()) {
        for (const Json& m : monitors.Items()) {
          if (m.BoolAt("disabled", false)) continue;
          int pw = static_cast<int>(m.NumberAt("width"));
          int ph = static_cast<int>(m.NumberAt("height"));
          if ((static_cast<int>(m.NumberAt("transform")) & 1) != 0) std::swap(pw, ph);
          const double scale = m.NumberAt("scale", 1.0) > 0 ? m.NumberAt("scale", 1.0) : 1.0;
          if (pw <= 0 || ph <= 0) continue;
          Display d;
          d.name = m.StringAt("name");
          d.pixel_w = pw;
          d.pixel_h = ph;
          d.lx = m.NumberAt("x");
          d.ly = m.NumberAt("y");
          d.lw = pw / scale;
          d.lh = ph / scale;
          displays.push_back(d);
        }
        if (!displays.empty()) {
          log.Info("Using Hyprland IPC monitor layout.");
          return true;
        }
      } else {
        log.Warn("Hyprland monitor query failed: " + error);
      }
    }
    if (session.sway) {
      Json outs;
      if (SwayQuery(kSwayGetOutputs, outs, error) && outs.IsArray()) {
        for (const Json& o : outs.Items()) {
          if (!o.BoolAt("active", true)) continue;
          const Json* rect = o.Get("rect");
          const Json* mode = o.Get("current_mode");
          if (rect == nullptr || mode == nullptr) continue;
          int pw = static_cast<int>(mode->NumberAt("width"));
          int ph = static_cast<int>(mode->NumberAt("height"));
          const std::string transform = o.StringAt("transform");
          if (transform.find("90") != std::string::npos || transform.find("270") != std::string::npos) {
            std::swap(pw, ph);
          }
          if (pw <= 0 || ph <= 0) continue;
          Display d;
          d.name = o.StringAt("name");
          d.pixel_w = pw;
          d.pixel_h = ph;
          d.lx = rect->NumberAt("x");
          d.ly = rect->NumberAt("y");
          d.lw = rect->NumberAt("width");
          d.lh = rect->NumberAt("height");
          displays.push_back(d);
        }
        if (!displays.empty()) {
          log.Info("Using sway IPC output layout.");
          return true;
        }
      } else {
        log.Warn("sway output query failed: " + error);
      }
    }
    return false;
  }

  static void AttachX11(Display& d, const X11Monitor& m, int order) {
    d.has_x11 = true;
    d.x11 = MakeRect(m.x, m.y, m.width, m.height);
    d.x11_output = m.output_id;
    d.x11_primary = m.primary;
    d.x11_order = order;
  }

  // XWayland mirrors wl_output names in RandR; fall back to sizes, then order.
  void MatchX11Monitors(const std::vector<X11Monitor>& monitors, const Log& log, bool reorder) {
    std::vector<bool> used(monitors.size(), false);
    for (size_t j = 0; j < monitors.size(); j++) {
      for (Display& d : displays) {
        if (!d.has_x11 && !d.name.empty() && d.name == monitors[j].name) {
          AttachX11(d, monitors[j], static_cast<int>(j));
          used[j] = true;
          break;
        }
      }
    }
    for (size_t j = 0; j < monitors.size(); j++) {
      if (used[j]) continue;
      for (Display& d : displays) {
        const bool same_pixels = monitors[j].width == d.pixel_w && monitors[j].height == d.pixel_h;
        const bool same_logical = std::fabs(monitors[j].width - d.lw) < 1 && std::fabs(monitors[j].height - d.lh) < 1;
        if (!d.has_x11 && (same_pixels || same_logical)) {
          AttachX11(d, monitors[j], static_cast<int>(j));
          used[j] = true;
          break;
        }
      }
    }
    for (size_t j = 0; j < monitors.size(); j++) {
      if (used[j]) continue;
      for (Display& d : displays) {
        if (!d.has_x11) {
          AttachX11(d, monitors[j], static_cast<int>(j));
          used[j] = true;
          break;
        }
      }
      if (!used[j]) log.Warn("X11 monitor " + monitors[j].name + " has no matching Wayland output; ignoring it.");
    }
    if (!reorder) return;
    std::stable_sort(displays.begin(), displays.end(), [](const Display& a, const Display& b) {
      const int ka = a.has_x11 ? a.x11_order : 1 << 30;
      const int kb = b.has_x11 ? b.x11_order : 1 << 30;
      return ka < kb;
    });
  }

  bool Init(const Log& log) {
    if (initialized) {
      if (!init_ok) log.Error("Screen capture is unavailable: " + init_error);
      return init_ok;
    }
    initialized = true;
    session = DetectSession();
    const std::string forced_raw = Env("PLAUD_SCREEN_BACKEND");
    Backend forced = Backend::Grim;
    const bool has_forced = !forced_raw.empty() && ParseBackend(forced_raw, forced);
    if (!forced_raw.empty() && !has_forced) {
      log.Warn("Ignoring unknown PLAUD_SCREEN_BACKEND=" + forced_raw + " (expected grim, portal or x11).");
    }
    grim_path = FindExecutable("grim");

    if (session.wayland) {
      if (!LoadWaylandDisplays(log)) {
        init_error = "cannot read the Wayland output layout";
        log.Error("Screen capture is unavailable: " + init_error);
        return false;
      }
      space = DecideSpace(host, space_reason);
      if (space == Space::X11) {
        std::string error;
        x11_geometry_tried = true;
        x11 = X11Connection::Open(error);
        if (x11) {
          MatchX11Monitors(x11->Monitors(), log, true);
        } else {
          log.Warn("XWayland geometry unavailable (" + error + "); using Wayland coordinates.");
          space = Space::Wayland;
          space_reason += "; XWayland unavailable";
        }
      }
      if (has_forced) {
        if (forced == Backend::X11) {
          init_error = "PLAUD_SCREEN_BACKEND=x11 cannot capture inside a Wayland session";
        } else if (forced == Backend::Grim && grim_path.empty()) {
          init_error = "PLAUD_SCREEN_BACKEND=grim but grim is not installed";
        } else if (forced == Backend::Grim && screencopy_known && !screencopy) {
          init_error = "PLAUD_SCREEN_BACKEND=grim but the compositor offers neither wlr-screencopy nor "
                       "ext-image-copy-capture";
        }
        if (!init_error.empty()) {
          log.Error("Screen capture is unavailable: " + init_error);
          return false;
        }
        backends.push_back(forced);
      } else {
        if (!grim_path.empty() && (screencopy || !screencopy_known)) backends.push_back(Backend::Grim);
        backends.push_back(Backend::Portal);
      }
    } else {
      std::string error;
      x11 = X11Connection::Open(error);
      if (!x11) {
        init_error = "no Wayland compositor and no X display (" + error + ")";
        log.Error("Screen capture is unavailable: " + init_error);
        return false;
      }
      if (x11->IsXWayland()) {
        init_error =
            "the X server is XWayland but no Wayland compositor socket was found; set WAYLAND_DISPLAY so capture "
            "goes through the compositor";
        log.Error("Screen capture is unavailable: " + init_error);
        return false;
      }
      space = Space::X11;
      space_reason = "X11 session";
      const std::vector<X11Monitor> monitors = x11->Monitors();
      for (size_t j = 0; j < monitors.size(); j++) {
        Display d;
        d.name = monitors[j].name;
        d.pixel_w = monitors[j].width;
        d.pixel_h = monitors[j].height;
        d.lx = monitors[j].x;
        d.ly = monitors[j].y;
        d.lw = monitors[j].width;
        d.lh = monitors[j].height;
        AttachX11(d, monitors[j], static_cast<int>(j));
        displays.push_back(d);
      }
      if (has_forced) {
        if (forced == Backend::Grim) {
          init_error = "PLAUD_SCREEN_BACKEND=grim needs a Wayland session";
          log.Error("Screen capture is unavailable: " + init_error);
          return false;
        }
        backends.push_back(forced);
      } else {
        backends.push_back(Backend::X11);
      }
    }
    if (displays.empty()) {
      init_error = "no displays found";
      log.Error("Screen capture is unavailable: " + init_error);
      return false;
    }
    std::string summary = std::string("Screen capture setup: session=") + (session.wayland ? "wayland" : "x11") +
                          ", coords=" + SpaceName(space) + " (" + space_reason + "), backends=";
    for (size_t i = 0; i < backends.size(); i++) summary += (i > 0 ? "," : "") + std::string(BackendName(backends[i]));
    summary += ", displays=[";
    for (size_t i = 0; i < displays.size(); i++) summary += (i > 0 ? "; " : "") + displays[i].Describe();
    summary += "]";
    log.Info(summary);
    init_ok = true;
    return true;
  }

  bool HasBackend(Backend b) const { return std::find(backends.begin(), backends.end(), b) != backends.end(); }

  // ---------------------------------------------------------- geometry

  IRect DisplayRect(const Display& d, Space s) const {
    if (s == Space::X11) return d.has_x11 ? d.x11 : IRect();
    return d.WaylandRect();
  }

  size_t MainIndex() const {
    if (space == Space::X11) {
      for (size_t i = 0; i < displays.size(); i++) {
        if (displays[i].has_x11 && displays[i].x11_primary) return i;
      }
    }
    for (size_t i = 0; i < displays.size(); i++) {
      if (displays[i].ContainsLogical(0, 0)) return i;
    }
    return 0;
  }

  int DisplayAtLogical(double x, double y) const {
    for (size_t i = 0; i < displays.size(); i++) {
      if (displays[i].ContainsLogical(x, y)) return static_cast<int>(i);
    }
    return -1;
  }

  // Display under the pointer; -1 when the compositor does not expose it.
  int CursorDisplay(const Log& log, std::string& where) {
    std::string error;
    if (session.wayland) {
      if (session.hyprland) {
        Json pos;
        if (HyprlandQuery("j/cursorpos", pos, error)) {
          const double x = pos.NumberAt("x");
          const double y = pos.NumberAt("y");
          where = "(" + Fmt(x) + ", " + Fmt(y) + ")";
          return DisplayAtLogical(x, y);
        }
        log.Warn("Hyprland cursor query failed: " + error);
      } else if (session.sway) {
        Json outs;
        if (SwayQuery(kSwayGetOutputs, outs, error) && outs.IsArray()) {
          for (const Json& o : outs.Items()) {
            if (!o.BoolAt("focused")) continue;
            const std::string name = o.StringAt("name");
            where = "focused output " + name;
            for (size_t i = 0; i < displays.size(); i++) {
              if (displays[i].name == name) return static_cast<int>(i);
            }
          }
        } else {
          log.Warn("sway output query failed: " + error);
        }
      }
      return -1;
    }
    int x = 0;
    int y = 0;
    if (x11 && x11->QueryPointer(x, y)) {
      where = "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
      for (size_t i = 0; i < displays.size(); i++) {
        const IRect& r = displays[i].x11;
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) return static_cast<int>(i);
      }
    }
    return -1;
  }

  // Maps a logical rectangle into this process's rect space.
  void LogicalToSpace(const FoundWindow& w, WindowInfo& out) const {
    int idx = DisplayAtLogical(w.x + w.w / 2, w.y + w.h / 2);
    if (idx < 0) idx = DisplayAtLogical(w.x, w.y);
    if (idx < 0) idx = static_cast<int>(MainIndex());
    const Display& d = displays[static_cast<size_t>(idx)];
    if (space == Space::X11 && d.has_x11) {
      const double kx = d.lw > 0 ? d.x11.w / d.lw : 1.0;
      const double ky = d.lh > 0 ? d.x11.h / d.lh : 1.0;
      out.x = d.x11.x + (w.x - d.lx) * kx;
      out.y = d.x11.y + (w.y - d.ly) * ky;
      out.width = w.w * kx;
      out.height = w.h * ky;
    } else {
      const double s = d.Scale();
      const IRect r = d.WaylandRect();
      out.x = r.x + (w.x - d.lx) * s;
      out.y = r.y + (w.y - d.ly) * s;
      out.width = w.w * s;
      out.height = w.h * s;
    }
  }

  double ScaleAt(const FoundWindow& w) const {
    int idx = DisplayAtLogical(w.x + w.w / 2, w.y + w.h / 2);
    if (idx < 0) idx = static_cast<int>(MainIndex());
    return displays[static_cast<size_t>(idx)].Scale();
  }

  // ------------------------------------------------------- pixel sources

  std::vector<std::string> HelperEnv() const {
    std::vector<std::string> env;
    if (!session.wayland_display.empty()) env.push_back("WAYLAND_DISPLAY=" + session.wayland_display);
    return env;
  }

  bool GrimOutput(const Display& d, Image& out, std::string& error) {
    if (grim_path.empty()) {
      error = "grim is not installed";
      return false;
    }
    char scale[32];
    std::snprintf(scale, sizeof(scale), "%.6g", d.Scale());
    std::vector<std::string> argv = {grim_path};
    if (!d.name.empty()) {
      argv.insert(argv.end(), {"-o", d.name});
    } else {
      char geometry[96];
      std::snprintf(geometry, sizeof(geometry), "%lld,%lld %lldx%lld", static_cast<long long>(std::llround(d.lx)),
                    static_cast<long long>(std::llround(d.ly)), static_cast<long long>(std::llround(d.lw)),
                    static_cast<long long>(std::llround(d.lh)));
      argv.insert(argv.end(), {"-g", geometry});
    }
    argv.insert(argv.end(), {"-s", scale, "-t", "ppm", "-"});
    const size_t cap = static_cast<size_t>(d.pixel_w + 64) * static_cast<size_t>(d.pixel_h + 64) * 6 + 4096;
    const ProcessResult r = RunProcess(argv, HelperEnv(), kGrimTimeoutMs, cap);
    if (!r.Succeeded()) {
      error = r.Describe();
      return false;
    }
    if (!ParsePpm(r.out.data(), r.out.size(), out, error)) return false;
    if (out.width != d.pixel_w || out.height != d.pixel_h) out = ResizeWhole(std::move(out), d.pixel_w, d.pixel_h);
    return true;
  }

  bool PortalDesktop(const Log& log, std::string& error) {
    if (portal_tried) {
      error = portal_error;
      return portal_ok;
    }
    portal_tried = true;
    std::string path;
    log.Info("Requesting a screenshot from xdg-desktop-portal.");
    if (!PortalScreenshot(path, portal_error, kPortalTimeoutMs)) {
      error = portal_error;
      return false;
    }
    std::vector<uint8_t> bytes;
    const bool read_ok = ReadFile(path, bytes, portal_error);
    if (IsTempPath(path)) {
      std::remove(path.c_str());
    } else {
      log.Warn("Portal saved the screenshot outside temporary directories; leaving " + path + " in place.");
    }
    if (!read_ok || !DecodePng(bytes.data(), bytes.size(), portal_image, portal_error)) {
      error = portal_error;
      return false;
    }
    portal_ok = true;
    return true;
  }

  // The portal returns one image of the whole logical desktop; cut the
  // output's logical rectangle out of it and bring it to native pixels.
  bool PortalOutput(const Display& d, Image& out, const Log& log, std::string& error) {
    if (!PortalDesktop(log, error)) return false;
    double x0 = displays[0].lx;
    double y0 = displays[0].ly;
    double x1 = displays[0].lx + displays[0].lw;
    double y1 = displays[0].ly + displays[0].lh;
    for (const Display& o : displays) {
      x0 = std::min(x0, o.lx);
      y0 = std::min(y0, o.ly);
      x1 = std::max(x1, o.lx + o.lw);
      y1 = std::max(y1, o.ly + o.lh);
    }
    if (x1 <= x0 || y1 <= y0) {
      error = "empty desktop layout";
      return false;
    }
    const double kx = portal_image.width / (x1 - x0);
    const double ky = portal_image.height / (y1 - y0);
    out.Allocate(d.pixel_w, d.pixel_h);
    ResampleInto(portal_image, (d.lx - x0) * kx, (d.ly - y0) * ky, d.lw * kx, d.lh * ky, out, 0, 0, d.pixel_w,
                 d.pixel_h);
    return true;
  }

  bool X11Output(const Display& d, Image& out, std::string& error) {
    if (!x11 || !d.has_x11) {
      error = "no X11 geometry for this display";
      return false;
    }
    int rw = 0;
    int rh = 0;
    x11->RootSize(rw, rh);
    const IRect r = Intersect(d.x11, MakeRect(0, 0, rw, rh));
    if (r.Empty()) {
      error = "display lies outside the X root window";
      return false;
    }
    Image grabbed;
    if (!x11->GrabRoot(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.w), static_cast<int>(r.h),
                       grabbed, error)) {
      return false;
    }
    out.Allocate(d.pixel_w, d.pixel_h);
    const double kx = static_cast<double>(d.pixel_w) / d.x11.w;
    const double ky = static_cast<double>(d.pixel_h) / d.x11.h;
    ResampleInto(grabbed, 0, 0, r.w, r.h, out, static_cast<int>(std::llround((r.x - d.x11.x) * kx)),
                 static_cast<int>(std::llround((r.y - d.x11.y) * ky)), static_cast<int>(std::llround(r.w * kx)),
                 static_cast<int>(std::llround(r.h * ky)));
    return true;
  }

  // Native-pixel image of display i, captured at most once per call.
  const Image* OutputImage(size_t i, const Log& log) {
    auto cached = outputs.find(i);
    if (cached != outputs.end()) return &cached->second;
    if (failed_outputs.count(i) != 0) {
      log.Error("Capture of display " + displays[i].name + " already failed in this call: " + failed_outputs[i]);
      return nullptr;
    }
    const Display& d = displays[i];
    std::string last_error;
    for (Backend b : backends) {
      Image img;
      std::string error;
      bool ok = false;
      switch (b) {
        case Backend::Grim:
          ok = GrimOutput(d, img, error);
          break;
        case Backend::Portal:
          ok = PortalOutput(d, img, log, error);
          break;
        case Backend::X11:
          ok = X11Output(d, img, error);
          break;
      }
      if (ok && !img.Empty()) return &(outputs[i] = std::move(img));
      last_error = std::string(BackendName(b)) + ": " + error;
      log.Warn("Capturing display " + d.name + " with " + BackendName(b) + " failed: " + error);
    }
    if (backends.empty()) last_error = "no capture backend available";
    failed_outputs[i] = last_error;
    log.Error("Failed to capture display " + d.name + " (" + last_error + ").");
    return nullptr;
  }

  // Composes rectangle `t` of space `s`; every covered output contributes its
  // part, resampled to one target pixel per space unit. Areas no display
  // covers stay black, as BitBlt from the virtual screen leaves them on Windows.
  bool ComposeSpace(Space s, const IRect& t, Image& dst, const Log& log) {
    dst.Allocate(static_cast<int>(t.w), static_cast<int>(t.h));
    bool any = false;
    for (size_t i = 0; i < displays.size(); i++) {
      const IRect dr = DisplayRect(displays[i], s);
      if (dr.Empty()) continue;
      const IRect part = Intersect(t, dr);
      if (part.Empty()) continue;
      const Image* src = OutputImage(i, log);
      if (src == nullptr) return false;
      const double kx = static_cast<double>(src->width) / dr.w;
      const double ky = static_cast<double>(src->height) / dr.h;
      ResampleInto(*src, (part.x - dr.x) * kx, (part.y - dr.y) * ky, part.w * kx, part.h * ky, dst,
                   static_cast<int>(part.x - t.x), static_cast<int>(part.y - t.y), static_cast<int>(part.w),
                   static_cast<int>(part.h));
      any = true;
    }
    if (!any) log.Info("Rect covers no display; the image is black.");
    return true;
  }

  // Composes a logical rectangle at `k` target pixels per logical unit.
  bool ComposeLogical(const FoundWindow& w, double k, Image& dst, const Log& log) {
    const long long out_w = std::max(1LL, std::llround(w.w * k));
    const long long out_h = std::max(1LL, std::llround(w.h * k));
    if (out_w > kMaxDimension || out_h > kMaxDimension) {
      log.Error("Window is too large to capture.");
      return false;
    }
    dst.Allocate(static_cast<int>(out_w), static_cast<int>(out_h));
    bool any = false;
    for (size_t i = 0; i < displays.size(); i++) {
      const Display& d = displays[i];
      const double ix0 = std::max(w.x, d.lx);
      const double iy0 = std::max(w.y, d.ly);
      const double ix1 = std::min(w.x + w.w, d.lx + d.lw);
      const double iy1 = std::min(w.y + w.h, d.ly + d.lh);
      if (ix1 <= ix0 || iy1 <= iy0) continue;
      const Image* src = OutputImage(i, log);
      if (src == nullptr) return false;
      const double sx = src->width / d.lw;
      const double sy = src->height / d.lh;
      const long long dx0 = std::llround((ix0 - w.x) * k);
      const long long dy0 = std::llround((iy0 - w.y) * k);
      const long long dx1 = std::llround((ix1 - w.x) * k);
      const long long dy1 = std::llround((iy1 - w.y) * k);
      ResampleInto(*src, (ix0 - d.lx) * sx, (iy0 - d.ly) * sy, (ix1 - ix0) * sx, (iy1 - iy0) * sy, dst,
                   static_cast<int>(dx0), static_cast<int>(dy0), static_cast<int>(dx1 - dx0),
                   static_cast<int>(dy1 - dy0));
      any = true;
    }
    if (!any) log.Error("Window lies outside every display.");
    return any;
  }

  // ------------------------------------------------------------ windows

  bool FindWindowAtCursor(FoundWindow& out, const Log& log) {
    std::string error;
    if (session.wayland) {
      if (session.hyprland) {
        Json pos;
        std::vector<HyprClient> clients;
        if (!HyprlandQuery("j/cursorpos", pos, error) || !LoadHyprClients(clients, error)) {
          log.Error("Hyprland IPC failed: " + error);
          return false;
        }
        const double cx = pos.NumberAt("x");
        const double cy = pos.NumberAt("y");
        const std::string at = "(" + Fmt(cx) + ", " + Fmt(cy) + ")";
        log.Info("Locating window under cursor at " + at + ".");
        const HyprClient* best = nullptr;
        for (const HyprClient& c : clients) {
          const FoundWindow& w = c.window;
          if (!c.candidate || cx < w.x || cy < w.y || cx >= w.x + w.w || cy >= w.y + w.h) continue;
          if (best == nullptr || c.rank > best->rank || (c.rank == best->rank && c.focus < best->focus)) best = &c;
        }
        if (best == nullptr) {
          log.Info("No suitable window found under cursor at " + at + ".");
          return false;
        }
        out = best->window;
        return true;
      }
      if (session.sway) {
        Json tree;
        if (!SwayQuery(kSwayGetTree, tree, error)) {
          log.Error("sway IPC failed: " + error);
          return false;
        }
        log.Info("sway does not expose the pointer position over IPC; using the focused window.");
        const Json* node = FindSwayNode(tree, [](const Json& n) { return IsSwayWindow(n) && n.BoolAt("focused"); });
        if (node == nullptr) {
          log.Info("No focused window found.");
          return false;
        }
        FillSwayWindow(*node, out);
        return true;
      }
      log.Warn("Window lookup needs Hyprland or sway IPC; this compositor is not supported.");
      return false;
    }
    X11Window xw;
    if (!x11 || !x11->WindowUnderPointer(xw, log)) return false;
    out.id = xw.id;
    out.x = xw.x;
    out.y = xw.y;
    out.w = xw.width;
    out.h = xw.height;
    out.owner = xw.owner;
    out.title = xw.title;
    return true;
  }

  bool FindWindowById(uint32_t id, FoundWindow& out, const Log& log) {
    std::string error;
    if (session.wayland) {
      if (session.hyprland) {
        std::vector<HyprClient> clients;
        if (!LoadHyprClients(clients, error)) {
          log.Error("Hyprland IPC failed: " + error);
          return false;
        }
        for (const HyprClient& c : clients) {
          if (c.window.id == id) {
            out = c.window;
            return true;
          }
        }
        return false;
      }
      if (session.sway) {
        Json tree;
        if (!SwayQuery(kSwayGetTree, tree, error)) {
          log.Error("sway IPC failed: " + error);
          return false;
        }
        const Json* node = FindSwayNode(tree, [id](const Json& n) {
          return IsSwayWindow(n) && static_cast<uint32_t>(static_cast<unsigned long long>(n.NumberAt("id")) &
                                                          0xffffffffULL) == id;
        });
        if (node == nullptr) return false;
        FillSwayWindow(*node, out);
        return true;
      }
      log.Warn("Window lookup needs Hyprland or sway IPC; this compositor is not supported.");
      return false;
    }
    X11Window xw;
    if (!x11 || !x11->WindowById(id, xw)) return false;
    out.id = xw.id;
    out.x = xw.x;
    out.y = xw.y;
    out.w = xw.width;
    out.h = xw.height;
    out.owner = xw.owner;
    out.title = xw.title;
    return true;
  }

  bool CaptureWindowImage(const FoundWindow& w, Image& img, const Log& log) {
    if (session.wayland) {
      if (!w.toplevel.empty() && HasBackend(Backend::Grim) && !grim_path.empty()) {
        const std::vector<std::string> argv = {grim_path, "-T", w.toplevel, "-t", "ppm", "-"};
        const ProcessResult r = RunProcess(argv, HelperEnv(), kGrimTimeoutMs, kMaxToplevelBytes);
        std::string error;
        if (r.Succeeded() && ParsePpm(r.out.data(), r.out.size(), img, error)) return true;
        log.Warn("grim toplevel capture failed (" + (r.Succeeded() ? error : r.Describe()) +
                 "); falling back to a crop from the screen.");
      }
      if (!w.on_screen) {
        log.Error("Window is not visible on screen; cannot crop it from the screen.");
        return false;
      }
      return ComposeLogical(w, ScaleAt(w), img, log);
    }
    int rw = 0;
    int rh = 0;
    x11->RootSize(rw, rh);
    const IRect r = Intersect(MakeRect(static_cast<long long>(w.x), static_cast<long long>(w.y),
                                       static_cast<long long>(w.w), static_cast<long long>(w.h)),
                              MakeRect(0, 0, rw, rh));
    if (r.Empty()) {
      log.Error("Window lies outside the screen.");
      return false;
    }
    if (HasBackend(Backend::X11)) {
      std::string error;
      if (x11->GrabRoot(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.w), static_cast<int>(r.h),
                        img, error)) {
        return true;
      }
      log.Error("XGetImage failed: " + error);
      return false;
    }
    return ComposeSpace(Space::X11, r, img, log);
  }

  // ---------------------------------------------------------- requests

  std::vector<uint8_t> Encode(const Image& img, const std::string& what, const Log& log) {
    std::vector<uint8_t> png;
    std::string error;
    if (!EncodePng(img, png, error)) {
      log.Error("Failed to encode " + what + " to PNG: " + error);
      return std::vector<uint8_t>();
    }
    return png;
  }

  std::vector<uint8_t> CaptureDisplayAt(size_t i, const Log& log) {
    const Image* img = OutputImage(i, log);
    if (img == nullptr) return std::vector<uint8_t>();
    std::vector<uint8_t> png = Encode(*img, "screenshot", log);
    if (!png.empty()) log.Info("Screenshot captured. PNG bytes: " + std::to_string(png.size()));
    return png;
  }

  std::vector<uint8_t> CaptureMain(const Log& log) {
    const size_t i = MainIndex();
    log.Info("Capturing main display screenshot (PNG). display=" + displays[i].name);
    return CaptureDisplayAt(i, log);
  }

  std::vector<uint8_t> CaptureCursorDisplay(const Log& log) {
    log.Info("Capturing cursor display screenshot (PNG)...");
    std::string where;
    int i = CursorDisplay(log, where);
    if (i < 0) {
      log.Warn(where.empty() ? "Cannot read the cursor position; falling back to main display."
                             : "Cursor at " + where + " is outside every display; falling back to main display.");
      i = static_cast<int>(MainIndex());
    } else {
      log.Info("Cursor at " + where + ", chosen display=" + displays[static_cast<size_t>(i)].name);
    }
    return CaptureDisplayAt(static_cast<size_t>(i), log);
  }

  std::vector<uint8_t> CaptureDisplayById(uint32_t id, const Log& log) {
    log.Info("Capturing display screenshot (PNG). displayId=" + std::to_string(id));
    int found = -1;
    for (size_t i = 0; i < displays.size() && found < 0; i++) {
      const Display& d = displays[i];
      if (space == Space::X11 ? (d.has_x11 && d.x11_output == id) : (d.wl_global != 0 && d.wl_global == id)) {
        found = static_cast<int>(i);
      }
    }
    if (found < 0) {
      log.Info("CaptureDisplayPngById: treating displayId as displayIndex on Linux.");
      if (id < displays.size()) {
        found = static_cast<int>(id);
      } else {
        log.Warn("displayId not found; falling back to primary display.");
        found = static_cast<int>(MainIndex());
      }
    }
    return CaptureDisplayAt(static_cast<size_t>(found), log);
  }

  std::vector<uint8_t> CaptureDisplayByIndex(uint32_t index, const Log& log) {
    log.Info("Capturing display screenshot (PNG). displayIndex=" + std::to_string(index));
    size_t i = index;
    if (index >= displays.size()) {
      log.Warn("displayIndex out of range; falling back to primary display.");
      i = MainIndex();
    }
    return CaptureDisplayAt(i, log);
  }

  std::vector<uint8_t> CaptureWindow(const FoundWindow& w, const Log& log) {
    Image img;
    if (!CaptureWindowImage(w, img, log)) return std::vector<uint8_t>();
    std::vector<uint8_t> png = Encode(img, "window screenshot", log);
    if (!png.empty()) {
      log.Info("Window screenshot captured. windowId=" + std::to_string(w.id) + " PNG bytes: " +
               std::to_string(png.size()));
    }
    return png;
  }

  std::vector<uint8_t> CaptureWindowById(uint32_t id, const Log& log) {
    log.Info("Capturing window screenshot (PNG). windowId=" + std::to_string(id));
    FoundWindow w;
    if (!FindWindowById(id, w, log)) {
      log.Error("CaptureWindowPngById: window not found. windowId=" + std::to_string(id));
      return std::vector<uint8_t>();
    }
    return CaptureWindow(w, log);
  }

  bool WindowTooSmall(const FoundWindow& w) const {
    const double s = session.wayland ? ScaleAt(w) : 1.0;
    return w.w * s < kMinWindowPixels || w.h * s < kMinWindowPixels;
  }

  std::vector<uint8_t> CaptureCursorWindow(const Log& log) {
    log.Info("Capturing cursor window screenshot (PNG)...");
    FoundWindow w;
    if (!FindWindowAtCursor(w, log)) {
      log.Error("Failed to detect a window under cursor.");
      return std::vector<uint8_t>();
    }
    if (WindowTooSmall(w)) {
      log.Info("Window under cursor is too small; ignoring.");
      log.Error("Failed to detect a window under cursor.");
      return std::vector<uint8_t>();
    }
    return CaptureWindow(w, log);
  }

  // XWayland geometry for a Wayland-coordinate process, loaded only when a rect
  // matched no Wayland display: covers an Electron running under XWayland
  // although the coordinate-space detection chose Wayland. Display order (and
  // the per-call output cache keyed by it) stays unchanged.
  void EnsureX11Geometry(const Log& log) {
    if (x11_geometry_tried || !session.wayland || Env("DISPLAY").empty()) return;
    x11_geometry_tried = true;
    std::string error;
    x11 = X11Connection::Open(error);
    if (x11) {
      MatchX11Monitors(x11->Monitors(), log, false);
    } else {
      log.Info("XWayland geometry unavailable (" + error + ").");
    }
  }

  // A rect naming a whole display in either coordinate space (default space
  // first) selects that display.
  int MatchDisplayRect(const IRect& r, std::string& how, const Log& log) {
    const std::vector<Space> order = {space, space == Space::X11 ? Space::Wayland : Space::X11};
    for (Space s : order) {
      if (s == Space::Wayland && !session.wayland) continue;
      if (s == Space::X11) EnsureX11Geometry(log);
      for (size_t i = 0; i < displays.size(); i++) {
        const Display& d = displays[i];
        if (s == Space::X11) {
          if (d.has_x11 && Near(r, d.x11)) {
            how = "X11 bounds";
            return static_cast<int>(i);
          }
        } else if (Near(r, d.WaylandRect())) {
          how = "Wayland bounds";
          return static_cast<int>(i);
        } else if (Near(r, d.ElectronWaylandRect())) {
          how = "Electron Wayland physicalBounds";
          return static_cast<int>(i);
        }
      }
    }
    return -1;
  }

  std::vector<uint8_t> CaptureRect(const CaptureRequest& req, const Log& log) {
    const IRect t = MakeRect(static_cast<long long>(std::floor(req.x)), static_cast<long long>(std::floor(req.y)), 0, 0);
    const long long right = static_cast<long long>(std::ceil(req.x + req.width));
    const long long bottom = static_cast<long long>(std::ceil(req.y + req.height));
    IRect want = MakeRect(t.x, t.y, right - t.x, bottom - t.y);
    std::string how;
    const int match = MatchDisplayRect(want, how, log);
    if (match >= 0) {
      const Display& d = displays[static_cast<size_t>(match)];
      log.Info("Rect matches display " + d.name + " (" + how + "); capturing the whole display.");
      const Image* img = OutputImage(static_cast<size_t>(match), log);
      if (img == nullptr) return std::vector<uint8_t>();
      std::vector<uint8_t> png;
      if (img->width == want.w && img->height == want.h) {
        png = Encode(*img, "rect screenshot", log);
      } else {
        Image copy = *img;
        png = Encode(ResizeWhole(std::move(copy), static_cast<int>(want.w), static_cast<int>(want.h)),
                     "rect screenshot", log);
      }
      if (!png.empty()) log.Info("Rect screenshot captured. PNG bytes: " + std::to_string(png.size()));
      return png;
    }
    bool have_bbox = false;
    IRect bbox;
    for (const Display& d : displays) {
      const IRect r = DisplayRect(d, space);
      if (r.Empty()) continue;
      if (!have_bbox) {
        bbox = r;
        have_bbox = true;
        continue;
      }
      const long long x0 = std::min(bbox.x, r.x);
      const long long y0 = std::min(bbox.y, r.y);
      const long long x1 = std::max(bbox.x + bbox.w, r.x + r.w);
      const long long y1 = std::max(bbox.y + bbox.h, r.y + r.h);
      bbox = MakeRect(x0, y0, x1 - x0, y1 - y0);
    }
    const IRect clipped = have_bbox ? Intersect(want, bbox) : IRect();
    if (clipped.Empty()) {
      log.Error("CaptureScreenRectPng: requested rect is outside the virtual screen.");
      return std::vector<uint8_t>();
    }
    if (clipped.w > kMaxDimension || clipped.h > kMaxDimension) {
      log.Error("CaptureScreenRectPng: invalid rect size.");
      return std::vector<uint8_t>();
    }
    Image img;
    if (!ComposeSpace(space, clipped, img, log)) return std::vector<uint8_t>();
    std::vector<uint8_t> png = Encode(img, "rect screenshot", log);
    if (!png.empty()) log.Info("Rect screenshot captured. PNG bytes: " + std::to_string(png.size()));
    return png;
  }
};

ScreenCapturer::ScreenCapturer(const HostInfo& host) : state_(new State(host)) {}

ScreenCapturer::~ScreenCapturer() = default;

std::vector<uint8_t> ScreenCapturer::Capture(const CaptureRequest& request, const Log& log) {
  try {
    if (request.kind == CaptureRequest::kRect) {
      log.Info("Capturing screen rect screenshot (PNG). rect=(" + Fmt(request.x) + ", " + Fmt(request.y) + ", " +
               Fmt(request.width) + ", " + Fmt(request.height) + ")");
      if (!std::isfinite(request.x) || !std::isfinite(request.y) || !std::isfinite(request.width) ||
          !std::isfinite(request.height) || std::fabs(request.x) > 1e9 || std::fabs(request.y) > 1e9 ||
          std::fabs(request.width) > 1e9 || std::fabs(request.height) > 1e9) {
        log.Error("CaptureScreenRectPng: non-finite rect values.");
        return std::vector<uint8_t>();
      }
      if (!(request.width > 0) || !(request.height > 0)) {
        log.Error("CaptureScreenRectPng: invalid rect size.");
        return std::vector<uint8_t>();
      }
    }
    if (!state_->Init(log)) return std::vector<uint8_t>();
    switch (request.kind) {
      case CaptureRequest::kMainDisplay:
        return state_->CaptureMain(log);
      case CaptureRequest::kCursorWindow:
        return state_->CaptureCursorWindow(log);
      case CaptureRequest::kDisplayById:
        return state_->CaptureDisplayById(request.display_id, log);
      case CaptureRequest::kDisplayByIndex:
        return state_->CaptureDisplayByIndex(request.display_index, log);
      case CaptureRequest::kWindowById:
        return state_->CaptureWindowById(request.window_id, log);
      case CaptureRequest::kRect:
        return state_->CaptureRect(request, log);
      default:
        return state_->CaptureCursorDisplay(log);
    }
  } catch (const std::exception& e) {
    log.Error(std::string("Exception during screenshot capture: ") + e.what());
  }
  return std::vector<uint8_t>();
}

bool ScreenCapturer::WindowAtCursor(WindowInfo& out, const Log& log) {
  try {
    if (!state_->Init(log)) return false;
    FoundWindow w;
    if (!state_->FindWindowAtCursor(w, log)) return false;
    if (state_->WindowTooSmall(w)) {
      log.Info("Window under cursor is too small; ignoring.");
      return false;
    }
    out.window_id = w.id;
    out.owner_name = w.owner;
    out.window_name = w.title;
    if (state_->session.wayland) {
      state_->LogicalToSpace(w, out);
    } else {
      out.x = w.x;
      out.y = w.y;
      out.width = w.w;
      out.height = w.h;
    }
    log.Info("Detected window under cursor. windowId=" + std::to_string(out.window_id) + " bounds=(" + Fmt(out.x) +
             ", " + Fmt(out.y) + ", " + Fmt(out.width) + ", " + Fmt(out.height) + ") owner=" + out.owner_name +
             " name=" + out.window_name);
    return true;
  } catch (const std::exception& e) {
    log.Error(std::string("Exception during GetCursorWindowInfo: ") + e.what());
  }
  return false;
}

bool ScreenCapturer::Usable(std::string& reason) {
  State& st = *state_;
  if (!st.Init(Log())) {
    reason = st.init_error;
    return false;
  }
  std::string reasons;
  if (st.session.wayland && !st.HasBackend(Backend::Grim) && Env("PLAUD_SCREEN_BACKEND").empty()) {
    reasons = st.grim_path.empty() ? "grim: not installed"
                                   : "grim: compositor offers neither wlr-screencopy nor ext-image-copy-capture";
  }
  for (Backend b : st.backends) {
    std::string error;
    switch (b) {
      case Backend::Grim:
      case Backend::X11:
        return true;  // Init verified the prerequisites (grim + capture protocol, X display)
      case Backend::Portal: {
        uint32_t version = 0;
        if (PortalScreenshotVersion(version, error)) return true;
        break;
      }
    }
    reasons += std::string(reasons.empty() ? "" : "; ") + BackendName(b) + ": " + error;
  }
  reason = reasons;
  return false;
}

PermissionStatus CheckScreenCapturePermission(const HostInfo& host, const Log& log) {
  log.Info("Starting screen capture permission check");
  ScreenCapturer capturer(host);
  std::string reason;
  if (capturer.Usable(reason)) {
    log.Info("Screen capture permission is not required on Linux; assuming granted.");
    return PermissionStatus::Granted;
  }
  log.Warn("No usable screen capture backend (" + reason + ").");
  return PermissionStatus::Denied;
}

}  // namespace screen
}  // namespace plaud_rec
