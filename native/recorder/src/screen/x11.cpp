// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "x11.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace plaud_rec {
namespace screen {

namespace {

// Function table resolved at runtime; X11 headers are used for types only.
struct XlibApi {
  bool ok = false;
  bool randr = false;
  std::string error;
  decltype(&::XOpenDisplay) OpenDisplay = nullptr;
  decltype(&::XCloseDisplay) CloseDisplay = nullptr;
  // Named *Fn: Xlib defines DefaultScreen/RootWindow/DisplayWidth/DisplayHeight as macros.
  decltype(&::XDefaultScreen) DefaultScreenFn = nullptr;
  decltype(&::XRootWindow) RootWindowFn = nullptr;
  decltype(&::XDisplayWidth) DisplayWidthFn = nullptr;
  decltype(&::XDisplayHeight) DisplayHeightFn = nullptr;
  decltype(&::XInternAtom) InternAtom = nullptr;
  decltype(&::XGetAtomName) GetAtomName = nullptr;
  decltype(&::XGetWindowProperty) GetWindowProperty = nullptr;
  decltype(&::XFree) Free = nullptr;
  decltype(&::XQueryPointer) QueryPointer = nullptr;
  decltype(&::XGetWindowAttributes) GetWindowAttributes = nullptr;
  decltype(&::XTranslateCoordinates) TranslateCoordinates = nullptr;
  decltype(&::XGetImage) GetImage = nullptr;
  decltype(&::XSetErrorHandler) SetErrorHandler = nullptr;
  decltype(&::XSync) Sync = nullptr;
  decltype(&::XQueryExtension) QueryExtension = nullptr;
  decltype(&::XQueryTree) QueryTree = nullptr;
  decltype(&::XRRQueryExtension) RRQueryExtension = nullptr;
  decltype(&::XRRQueryVersion) RRQueryVersion = nullptr;
  decltype(&::XRRGetMonitors) RRGetMonitors = nullptr;
  decltype(&::XRRFreeMonitors) RRFreeMonitors = nullptr;
  decltype(&::XRRGetScreenResourcesCurrent) RRGetScreenResourcesCurrent = nullptr;
  decltype(&::XRRFreeScreenResources) RRFreeScreenResources = nullptr;
  decltype(&::XRRGetOutputInfo) RRGetOutputInfo = nullptr;
  decltype(&::XRRFreeOutputInfo) RRFreeOutputInfo = nullptr;
  decltype(&::XRRGetCrtcInfo) RRGetCrtcInfo = nullptr;
  decltype(&::XRRFreeCrtcInfo) RRFreeCrtcInfo = nullptr;
  decltype(&::XRRGetOutputPrimary) RRGetOutputPrimary = nullptr;
};

template <typename T>
bool Resolve(void* lib, const char* name, T& fn) {
  fn = reinterpret_cast<T>(dlsym(lib, name));
  return fn != nullptr;
}

const XlibApi& Api() {
  static const XlibApi api = [] {
    XlibApi a;
    void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (x11 == nullptr) {
      const char* e = dlerror();
      a.error = std::string("dlopen(libX11.so.6) failed: ") + (e != nullptr ? e : "unknown error");
      return a;
    }
    a.ok = Resolve(x11, "XOpenDisplay", a.OpenDisplay) && Resolve(x11, "XCloseDisplay", a.CloseDisplay) &&
           Resolve(x11, "XDefaultScreen", a.DefaultScreenFn) && Resolve(x11, "XRootWindow", a.RootWindowFn) &&
           Resolve(x11, "XDisplayWidth", a.DisplayWidthFn) && Resolve(x11, "XDisplayHeight", a.DisplayHeightFn) &&
           Resolve(x11, "XInternAtom", a.InternAtom) && Resolve(x11, "XGetAtomName", a.GetAtomName) &&
           Resolve(x11, "XGetWindowProperty", a.GetWindowProperty) && Resolve(x11, "XFree", a.Free) &&
           Resolve(x11, "XQueryPointer", a.QueryPointer) &&
           Resolve(x11, "XGetWindowAttributes", a.GetWindowAttributes) &&
           Resolve(x11, "XTranslateCoordinates", a.TranslateCoordinates) && Resolve(x11, "XGetImage", a.GetImage) &&
           Resolve(x11, "XSetErrorHandler", a.SetErrorHandler) && Resolve(x11, "XSync", a.Sync) &&
           Resolve(x11, "XQueryExtension", a.QueryExtension) && Resolve(x11, "XQueryTree", a.QueryTree);
    if (!a.ok) {
      a.error = "libX11.so.6 lacks required symbols";
      return a;
    }
    void* xrandr = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
    if (xrandr != nullptr) {
      a.randr = Resolve(xrandr, "XRRQueryExtension", a.RRQueryExtension) &&
                Resolve(xrandr, "XRRQueryVersion", a.RRQueryVersion) &&
                Resolve(xrandr, "XRRGetScreenResourcesCurrent", a.RRGetScreenResourcesCurrent) &&
                Resolve(xrandr, "XRRFreeScreenResources", a.RRFreeScreenResources) &&
                Resolve(xrandr, "XRRGetOutputInfo", a.RRGetOutputInfo) &&
                Resolve(xrandr, "XRRFreeOutputInfo", a.RRFreeOutputInfo) &&
                Resolve(xrandr, "XRRGetCrtcInfo", a.RRGetCrtcInfo) &&
                Resolve(xrandr, "XRRFreeCrtcInfo", a.RRFreeCrtcInfo) &&
                Resolve(xrandr, "XRRGetOutputPrimary", a.RRGetOutputPrimary);
      // RandR 1.5 monitor API is optional (older libXrandr).
      Resolve(xrandr, "XRRGetMonitors", a.RRGetMonitors);
      Resolve(xrandr, "XRRFreeMonitors", a.RRFreeMonitors);
    }
    return a;
  }();
  return api;
}

int g_x_error = 0;

int TrapHandler(Display*, XErrorEvent* event) {
  g_x_error = event->error_code;
  return 0;
}

// Xlib's error handler is process-global: install ours only for the duration
// of a query and restore the previous one (GTK's, when Electron runs on X11).
class ErrorTrap {
 public:
  explicit ErrorTrap(Display* d) : d_(d) {
    Api().Sync(d_, False);
    g_x_error = 0;
    previous_ = Api().SetErrorHandler(TrapHandler);
  }
  ~ErrorTrap() {
    Api().Sync(d_, False);
    Api().SetErrorHandler(previous_);
  }
  bool Failed() {
    Api().Sync(d_, False);
    return g_x_error != 0;
  }

 private:
  Display* d_;
  XErrorHandler previous_ = nullptr;
};

Display* D(void* p) { return static_cast<Display*>(p); }

Window RootOf(Display* d) { return Api().RootWindowFn(d, Api().DefaultScreenFn(d)); }

Atom InternAtom(Display* d, const char* name) { return Api().InternAtom(d, name, False); }

// Reads a property; 32-bit formats arrive as arrays of C long (Xlib ABI).
bool GetProperty(Display* d, Window w, Atom prop, Atom type, std::vector<unsigned char>& data, int& format,
                 unsigned long& items, Atom* actual_type = nullptr) {
  Atom actual = 0;
  unsigned long after = 0;
  unsigned char* raw = nullptr;
  format = 0;
  items = 0;
  if (Api().GetWindowProperty(d, w, prop, 0, 1 << 16, False, type, &actual, &format, &items, &after, &raw) !=
          Success ||
      raw == nullptr) {
    return false;
  }
  if (actual_type != nullptr) *actual_type = actual;
  const size_t unit = format == 32 ? sizeof(long) : (format == 16 ? sizeof(short) : 1);
  data.assign(raw, raw + unit * items);
  Api().Free(raw);
  return actual != 0;
}

std::vector<unsigned long> GetLongs(Display* d, Window w, Atom prop, Atom type) {
  std::vector<unsigned char> data;
  int format = 0;
  unsigned long items = 0;
  std::vector<unsigned long> out;
  if (!GetProperty(d, w, prop, type, data, format, items) || format != 32) return out;
  out.resize(items);
  for (unsigned long i = 0; i < items; i++) {
    long v = 0;
    std::memcpy(&v, data.data() + i * sizeof(long), sizeof(long));
    out[i] = static_cast<unsigned long>(v);
  }
  return out;
}

std::string GetText(Display* d, Window w, Atom prop, Atom type) {
  std::vector<unsigned char> data;
  int format = 0;
  unsigned long items = 0;
  if (!GetProperty(d, w, prop, type, data, format, items) || format != 8) return std::string();
  return std::string(data.begin(), data.end());
}

std::string WindowTitle(Display* d, Window w) {
  std::string t = GetText(d, w, InternAtom(d, "_NET_WM_NAME"), InternAtom(d, "UTF8_STRING"));
  if (t.empty()) t = GetText(d, w, XA_WM_NAME, AnyPropertyType);
  return t;
}

std::string WindowClass(Display* d, Window w) {
  const std::string raw = GetText(d, w, XA_WM_CLASS, XA_STRING);
  // "instance\0class\0"
  const size_t nul = raw.find('\0');
  if (nul == std::string::npos) return raw;
  std::string cls = raw.substr(nul + 1);
  const size_t end = cls.find('\0');
  if (end != std::string::npos) cls.resize(end);
  return cls.empty() ? raw.substr(0, nul) : cls;
}

bool HasWmState(Display* d, Window w) {
  std::vector<unsigned char> data;
  int format = 0;
  unsigned long items = 0;
  const Atom wm_state = InternAtom(d, "WM_STATE");
  return GetProperty(d, w, wm_state, wm_state, data, format, items);
}

// Finds the client (WM_STATE-carrying) window below a top-level frame.
Window FindClient(Display* d, Window w, int depth) {
  if (HasWmState(d, w)) return w;
  if (depth >= 4) return 0;
  Window root = 0;
  Window parent = 0;
  Window* children = nullptr;
  unsigned int n = 0;
  if (Api().QueryTree(d, w, &root, &parent, &children, &n) == 0) return 0;
  Window found = 0;
  for (unsigned int i = n; i-- > 0 && found == 0;) found = FindClient(d, children[i], depth + 1);
  if (children != nullptr) Api().Free(children);
  return found;
}

bool OuterGeometry(Display* d, Window root, Window w, X11Window& out, bool& viewable) {
  XWindowAttributes attrs{};
  if (Api().GetWindowAttributes(d, w, &attrs) == 0) return false;
  viewable = attrs.map_state == IsViewable;
  int rx = 0;
  int ry = 0;
  Window child = 0;
  if (!Api().TranslateCoordinates(d, w, root, 0, 0, &rx, &ry, &child)) return false;
  const std::vector<unsigned long> ext = GetLongs(d, w, InternAtom(d, "_NET_FRAME_EXTENTS"), XA_CARDINAL);
  long left = 0;
  long right = 0;
  long top = 0;
  long bottom = 0;
  if (ext.size() >= 4) {
    left = static_cast<long>(ext[0]);
    right = static_cast<long>(ext[1]);
    top = static_cast<long>(ext[2]);
    bottom = static_cast<long>(ext[3]);
  }
  out.id = static_cast<uint32_t>(w);
  out.x = rx - static_cast<int>(left);
  out.y = ry - static_cast<int>(top);
  out.width = attrs.width + static_cast<int>(left + right);
  out.height = attrs.height + static_cast<int>(top + bottom);
  out.owner = WindowClass(d, w);
  out.title = WindowTitle(d, w);
  return true;
}

bool HasAtom(const std::vector<unsigned long>& list, Atom a) {
  return std::find(list.begin(), list.end(), static_cast<unsigned long>(a)) != list.end();
}

struct ChannelMask {
  unsigned long mask = 0;
  int shift = 0;
  int bits = 0;
  explicit ChannelMask(unsigned long m) : mask(m) {
    if (m == 0) return;
    while (((m >> shift) & 1ul) == 0) shift++;
    while (((m >> (shift + bits)) & 1ul) != 0) bits++;
  }
  uint8_t Extract(unsigned long pixel) const {
    if (bits == 0) return 0;
    const unsigned long v = (pixel & mask) >> shift;
    if (bits == 8) return static_cast<uint8_t>(v);
    const unsigned long max = (1ul << bits) - 1;
    return static_cast<uint8_t>((v * 255 + max / 2) / max);
  }
};

}  // namespace

std::unique_ptr<X11Connection> X11Connection::Open(std::string& error) {
  const XlibApi& api = Api();
  if (!api.ok) {
    error = api.error;
    return nullptr;
  }
  Display* d = api.OpenDisplay(nullptr);
  if (d == nullptr) {
    const char* name = std::getenv("DISPLAY");
    error = std::string("cannot open X display ") + (name != nullptr ? name : "(DISPLAY unset)");
    return nullptr;
  }
  return std::unique_ptr<X11Connection>(new X11Connection(d));
}

X11Connection::~X11Connection() {
  if (display_ != nullptr) Api().CloseDisplay(D(display_));
}

bool X11Connection::IsXWayland() const {
  int opcode = 0;
  int event = 0;
  int err = 0;
  return Api().QueryExtension(D(display_), "XWAYLAND", &opcode, &event, &err) != 0;
}

void X11Connection::RootSize(int& width, int& height) const {
  Display* d = D(display_);
  const int screen = Api().DefaultScreenFn(d);
  width = Api().DisplayWidthFn(d, screen);
  height = Api().DisplayHeightFn(d, screen);
}

std::vector<X11Monitor> X11Connection::Monitors() const {
  const XlibApi& api = Api();
  Display* d = D(display_);
  const Window root = RootOf(d);
  std::vector<X11Monitor> out;
  int event_base = 0;
  int error_base = 0;
  int major = 0;
  int minor = 0;
  const bool have_randr = api.randr && api.RRQueryExtension(d, &event_base, &error_base) &&
                          api.RRQueryVersion(d, &major, &minor) != 0;
  if (have_randr && api.RRGetMonitors != nullptr && (major > 1 || (major == 1 && minor >= 5))) {
    ErrorTrap trap(d);
    int n = 0;
    XRRMonitorInfo* m = api.RRGetMonitors(d, root, True, &n);
    for (int i = 0; m != nullptr && i < n; i++) {
      X11Monitor mon;
      char* name = api.GetAtomName(d, m[i].name);
      if (name != nullptr) {
        mon.name = name;
        api.Free(name);
      }
      mon.x = m[i].x;
      mon.y = m[i].y;
      mon.width = m[i].width;
      mon.height = m[i].height;
      mon.primary = m[i].primary != 0;
      mon.output_id = m[i].noutput > 0 ? static_cast<uint32_t>(m[i].outputs[0]) : 0;
      if (mon.width > 0 && mon.height > 0) out.push_back(mon);
    }
    if (m != nullptr) api.RRFreeMonitors(m);
    if (!out.empty()) return out;
  }
  if (have_randr) {
    ErrorTrap trap(d);
    XRRScreenResources* res = api.RRGetScreenResourcesCurrent(d, root);
    const RROutput primary = api.RRGetOutputPrimary(d, root);
    for (int i = 0; res != nullptr && i < res->noutput; i++) {
      XRROutputInfo* oi = api.RRGetOutputInfo(d, res, res->outputs[i]);
      if (oi == nullptr) continue;
      if (oi->crtc != 0 && oi->connection == RR_Connected) {
        XRRCrtcInfo* ci = api.RRGetCrtcInfo(d, res, oi->crtc);
        if (ci != nullptr) {
          X11Monitor mon;
          mon.name.assign(oi->name, static_cast<size_t>(oi->nameLen));
          mon.x = ci->x;
          mon.y = ci->y;
          mon.width = static_cast<int>(ci->width);
          mon.height = static_cast<int>(ci->height);
          mon.primary = res->outputs[i] == primary;
          mon.output_id = static_cast<uint32_t>(res->outputs[i]);
          if (mon.width > 0 && mon.height > 0) out.push_back(mon);
          api.RRFreeCrtcInfo(ci);
        }
      }
      api.RRFreeOutputInfo(oi);
    }
    if (res != nullptr) api.RRFreeScreenResources(res);
    if (!out.empty()) return out;
  }
  X11Monitor whole;
  whole.name = "screen";
  RootSize(whole.width, whole.height);
  whole.primary = true;
  out.push_back(whole);
  return out;
}

bool X11Connection::QueryPointer(int& x, int& y) const {
  const XlibApi& api = Api();
  Display* d = D(display_);
  const Window root = RootOf(d);
  Window root_ret = 0;
  Window child = 0;
  int wx = 0;
  int wy = 0;
  unsigned int mask = 0;
  return api.QueryPointer(d, root, &root_ret, &child, &x, &y, &wx, &wy, &mask) != 0;
}

bool X11Connection::WindowUnderPointer(X11Window& out, const Log& log) const {
  const XlibApi& api = Api();
  Display* d = D(display_);
  ErrorTrap trap(d);
  const Window root = RootOf(d);
  Window root_ret = 0;
  Window child = 0;
  int px = 0;
  int py = 0;
  int wx = 0;
  int wy = 0;
  unsigned int mask = 0;
  if (api.QueryPointer(d, root, &root_ret, &child, &px, &py, &wx, &wy, &mask) == 0) {
    log.Warn("XQueryPointer failed; cannot read cursor position.");
    return false;
  }
  const std::string at = "(" + std::to_string(px) + ", " + std::to_string(py) + ")";
  log.Info("Locating window under cursor at " + at + " (X11).");
  std::vector<unsigned long> stack = GetLongs(d, root, InternAtom(d, "_NET_CLIENT_LIST_STACKING"), XA_WINDOW);
  if (stack.empty()) stack = GetLongs(d, root, InternAtom(d, "_NET_CLIENT_LIST"), XA_WINDOW);
  const std::vector<unsigned long> current =
      GetLongs(d, root, InternAtom(d, "_NET_CURRENT_DESKTOP"), XA_CARDINAL);
  const Atom wm_state = InternAtom(d, "_NET_WM_STATE");
  const Atom hidden = InternAtom(d, "_NET_WM_STATE_HIDDEN");
  const Atom wm_type = InternAtom(d, "_NET_WM_WINDOW_TYPE");
  const Atom type_desktop = InternAtom(d, "_NET_WM_WINDOW_TYPE_DESKTOP");
  const Atom type_dock = InternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK");
  const Atom wm_desktop = InternAtom(d, "_NET_WM_DESKTOP");

  auto accept = [&](Window w, X11Window& info) -> int {  // 1 found, 0 skip, -1 stop (shell window)
    bool viewable = false;
    if (!OuterGeometry(d, root, w, info, viewable) || !viewable) return 0;
    if (px < info.x || py < info.y || px >= info.x + info.width || py >= info.y + info.height) return 0;
    const std::vector<unsigned long> desk = GetLongs(d, w, wm_desktop, XA_CARDINAL);
    if (!desk.empty() && !current.empty() && desk[0] != 0xFFFFFFFFul && desk[0] != current[0]) return 0;
    if (HasAtom(GetLongs(d, w, wm_state, XA_ATOM), hidden)) return 0;
    const std::vector<unsigned long> types = GetLongs(d, w, wm_type, XA_ATOM);
    if (HasAtom(types, type_desktop) || HasAtom(types, type_dock)) return -1;
    return 1;
  };

  X11Window info;
  int verdict = 0;
  for (auto it = stack.rbegin(); it != stack.rend() && verdict == 0; ++it) {
    verdict = accept(static_cast<Window>(*it), info);
  }
  if (stack.empty() && child != 0) {
    const Window client = FindClient(d, child, 0);
    if (client != 0) verdict = accept(client, info);
  }
  if (verdict < 0) {
    log.Info("Cursor is over desktop/shell window; ignoring.");
    return false;
  }
  if (verdict == 0) {
    log.Info("No suitable window found under cursor at " + at + ".");
    return false;
  }
  if (info.width < 50 || info.height < 50) {
    log.Info("Window under cursor is too small; ignoring.");
    return false;
  }
  out = info;
  return true;
}

bool X11Connection::WindowById(uint32_t id, X11Window& out) const {
  Display* d = D(display_);
  ErrorTrap trap(d);
  const Window root = RootOf(d);
  bool viewable = false;
  X11Window info;
  if (!OuterGeometry(d, root, static_cast<Window>(id), info, viewable) || trap.Failed() || !viewable) return false;
  out = info;
  return true;
}

bool X11Connection::GrabRoot(int x, int y, int width, int height, Image& out, std::string& error) const {
  const XlibApi& api = Api();
  Display* d = D(display_);
  ErrorTrap trap(d);
  const Window root = RootOf(d);
  XImage* img = api.GetImage(d, root, x, y, static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                             AllPlanes, ZPixmap);
  if (img == nullptr || trap.Failed()) {
    if (img != nullptr) img->f.destroy_image(img);
    error = "XGetImage failed on the root window";
    return false;
  }
  Image result;
  result.Allocate(width, height);
  const ChannelMask r(img->red_mask);
  const ChannelMask g(img->green_mask);
  const ChannelMask b(img->blue_mask);
  const bool fast = (img->bits_per_pixel == 32 || img->bits_per_pixel == 24) && img->byte_order == LSBFirst;
  for (int row = 0; row < height; row++) {
    uint8_t* dst = result.rgb.data() + static_cast<size_t>(row) * width * 3;
    const uint8_t* line = reinterpret_cast<const uint8_t*>(img->data) + static_cast<size_t>(row) * img->bytes_per_line;
    for (int col = 0; col < width; col++) {
      unsigned long px;
      if (fast) {
        const uint8_t* p = line + static_cast<size_t>(col) * (img->bits_per_pixel / 8);
        px = static_cast<unsigned long>(p[0]) | (static_cast<unsigned long>(p[1]) << 8) |
             (static_cast<unsigned long>(p[2]) << 16) |
             (img->bits_per_pixel == 32 ? static_cast<unsigned long>(p[3]) << 24 : 0ul);
      } else {
        px = img->f.get_pixel(img, col, row);
      }
      dst[col * 3] = r.Extract(px);
      dst[col * 3 + 1] = g.Extract(px);
      dst[col * 3 + 2] = b.Extract(px);
    }
  }
  img->f.destroy_image(img);
  out = std::move(result);
  return true;
}

}  // namespace screen
}  // namespace plaud_rec
