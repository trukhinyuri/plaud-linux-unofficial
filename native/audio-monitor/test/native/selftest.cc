// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Pure-logic tests of the AudioMonitor core (no PipeWire needed), run by
// test/selftest.test.mjs. Prints "ok <group>" / "not ok <group>" lines and
// exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "app_catalog.h"
#include "device_filter.h"
#include "json.h"
#include "level_meter.h"
#include "meeting_state.h"
#include "proc_util.h"
#include "speaker_selector.h"
#include "voice_selector.h"
#include "webrtc_evidence.h"
#include "window_titles.h"

using namespace am;

namespace {

int gGroupFailures = 0;
int gTotalFailures = 0;
int gChecks = 0;

void fail(const char* file, int line, const std::string& what) {
  ++gGroupFailures;
  ++gTotalFailures;
  std::printf("#   FAIL %s:%d: %s\n", file, line, what.c_str());
}

#define CHECK(cond)                                   \
  do {                                                \
    ++gChecks;                                        \
    if (!(cond)) fail(__FILE__, __LINE__, #cond);     \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    ++gChecks;                                                                                      \
    const auto va = (a);                                                                            \
    const auto vb = (b);                                                                            \
    if (!(va == vb)) fail(__FILE__, __LINE__, std::string(#a " == " #b " (got '") + toStr(va) + "')"); \
  } while (0)

std::string toStr(const std::string& s) { return s; }
template <typename T>
std::string toStr(const T& v) {
  return std::to_string(v);
}

template <typename Body>
void group(const char* name, const Body& body) {
  gGroupFailures = 0;
  body();
  std::printf("%s %s\n", gGroupFailures == 0 ? "ok" : "not ok", name);
}

// --- helpers -----------------------------------------------------------------

std::vector<VoiceSelector::Event> tickVoice(VoiceSelector& s, int64_t t, std::vector<DeviceLevel> levels) {
  std::vector<VoiceSelector::Event> ev;
  s.evaluate(t, levels, ev);
  return ev;
}

bool hasEvent(const std::vector<VoiceSelector::Event>& ev, VoiceSelector::EventType type, const std::string& uid = "*") {
  for (const auto& e : ev) {
    if (e.type == type && (uid == "*" || e.uid == uid)) return true;
  }
  return false;
}

AppObservation obs(const std::string& app, bool browser, bool udp, bool title, std::set<uint64_t> streams) {
  AppObservation o;
  o.appName = app;
  o.browser = browser;
  o.udpEvidence = udp;
  o.titleEvidence = title;
  o.streams = std::move(streams);
  o.meta["processName"] = app;
  return o;
}

bool hasLog(const MeetingOutput& out, const std::string& message) {
  for (const auto& l : out.logs) {
    if (l.message == message) return true;
  }
  return false;
}

// The JS side (knownAppNames, first key contained in the lower-cased
// appName, in insertion order) -> realName.
std::string jsRealName(const std::string& appName) {
  static const std::vector<std::pair<std::string, std::string>> known = {
      {"zoom", "zoom"},          {"google-meet", "google-meet"},  {"msteams", "teams"},
      {"ms-teams", "teams"},     {"slack", "slack"},              {"discord", "discord"},
      {"chrome", "chrome"},      {"firefox", "firefox"},          {"arc", "arc"},
      {"webex", "webex"},        {"ciscocollabhost.exe", "webex"}, {"safari", "safari"},
      {"com.apple.webkit.gpu", "safari"}, {"whatsapp", "whatsapp"}, {"aircall", "aircall"},
      {"msedge", "msedge"},      {"tencentmeeting", "tencent"},   {"wemeetapp.exe", "tencent"},
      {"tuple", "tuple"},        {"brave", "brave"},              {"lark", "lark"},
      {"feishu", "lark"},        {"dingmeeting", "dingding"},     {"tblive.exe", "dingding"},
      {"huaweimeetingapp.exe", "huawei"}, {"welinkmeeting", "huawei"}};
  const std::string lower = toLower(appName);
  for (const auto& kv : known) {
    if (lower.find(kv.first) != std::string::npos) return kv.second;
  }
  return "";
}

bool jsIsBrowser(const std::string& realName) {
  return realName == "chrome" || realName == "firefox" || realName == "arc" || realName == "safari" ||
         realName == "msedge";
}

}  // namespace

int main() {
  group("voice-selector: switches after 5 wins (Windows ratio rule)", [] {
    VoiceSelector s;
    s.setCurrent("A");
    for (int i = 0; i < 4; ++i) {
      auto ev = tickVoice(s, i * 200, {{"A", "Mic A", 0.002f}, {"B", "Mic B", 0.02f}});
      CHECK(!hasEvent(ev, VoiceSelector::EventType::AutoSelected));
    }
    auto ev = tickVoice(s, 800, {{"A", "Mic A", 0.002f}, {"B", "Mic B", 0.02f}});
    CHECK(hasEvent(ev, VoiceSelector::EventType::AutoSelected, "B"));
    CHECK_EQ(s.current(), std::string("B"));

    // 3 s minimum between switches.
    for (int64_t t = 1000; t < 3800; t += 200) {
      ev = tickVoice(s, t, {{"A", "Mic A", 0.05f}, {"B", "Mic B", 0.002f}});
      CHECK(!hasEvent(ev, VoiceSelector::EventType::AutoSelected));
    }
    for (int64_t t = 3800; t < 4600; t += 200) {
      ev = tickVoice(s, t, {{"A", "Mic A", 0.05f}, {"B", "Mic B", 0.002f}});
      CHECK(!hasEvent(ev, VoiceSelector::EventType::AutoSelected));
    }
    ev = tickVoice(s, 4600, {{"A", "Mic A", 0.05f}, {"B", "Mic B", 0.002f}});
    CHECK(hasEvent(ev, VoiceSelector::EventType::AutoSelected, "A"));
  });

  group("voice-selector: ratio threshold 1.3 + 0.7 * clamp(cur/0.01)", [] {
    VoiceSelector s;
    s.setCurrent("A");
    int64_t t = 0;
    for (int i = 0; i < 12; ++i, t += 200) {
      auto ev = tickVoice(s, t, {{"A", "A", 0.01f}, {"B", "B", 0.019f}});  // ratio 1.9 < 2.0
      CHECK(!hasEvent(ev, VoiceSelector::EventType::AutoSelected));
    }
    bool switched = false;
    for (int i = 0; i < 5; ++i, t += 200) {
      auto ev = tickVoice(s, t, {{"A", "A", 0.01f}, {"B", "B", 0.021f}});  // ratio 2.1 >= 2.0
      switched = hasEvent(ev, VoiceSelector::EventType::AutoSelected, "B");
      if (i < 4) CHECK(!switched);
    }
    CHECK(switched);

    VoiceSelector q;  // current at 0.005 -> norm 0.5 -> threshold 1.65
    q.setCurrent("A");
    bool any = false;
    for (int i = 0; i < 10; ++i) {
      const auto ev = tickVoice(q, i * 200, {{"A", "A", 0.005f}, {"B", "B", 0.008f}});  // ratio 1.6
      if (hasEvent(ev, VoiceSelector::EventType::AutoSelected)) any = true;
    }
    CHECK(!any);
    VoiceSelector r;
    r.setCurrent("A");
    for (int i = 0; i < 5; ++i) {
      const auto ev = tickVoice(r, i * 200, {{"A", "A", 0.005f}, {"B", "B", 0.0085f}});  // ratio 1.7
      any = hasEvent(ev, VoiceSelector::EventType::AutoSelected);
    }
    CHECK(any);
  });

  group("voice-selector: silence and window expiry", [] {
    VoiceSelector s;
    s.setCurrent("A");
    for (int i = 0; i < 20; ++i) {
      auto ev = tickVoice(s, i * 200, {{"A", "A", 0.0005f}, {"B", "B", 0.0009f}});
      CHECK(!hasEvent(ev, VoiceSelector::EventType::AutoSelected));
    }
    VoiceSelector slow;  // 450 ms apart: at most 4 entries in 1.6 s
    slow.setCurrent("A");
    for (int i = 0; i < 20; ++i) {
      auto ev = tickVoice(slow, i * 450, {{"A", "A", 0.0f}, {"B", "B", 0.01f}});
      CHECK(!hasEvent(ev, VoiceSelector::EventType::AutoSelected));
    }
    VoiceSelector edge;  // 400 ms apart: entry exactly 1.6 s old still counts
    edge.setCurrent("A");
    bool switched = false;
    for (int i = 0; i < 5; ++i) {
      switched = hasEvent(tickVoice(edge, i * 400, {{"A", "A", 0.0f}, {"B", "B", 0.01f}}),
                          VoiceSelector::EventType::AutoSelected, "B");
    }
    CHECK(switched);
  });

  group("voice-selector: candidate change restarts the window", [] {
    VoiceSelector s;
    s.setCurrent("A");
    int64_t t = 0;
    for (int i = 0; i < 3; ++i, t += 200) tickVoice(s, t, {{"A", "A", 0.0f}, {"B", "B", 0.01f}, {"C", "C", 0.0f}});
    for (int i = 0; i < 4; ++i, t += 200) {
      auto ev = tickVoice(s, t, {{"A", "A", 0.0f}, {"B", "B", 0.0f}, {"C", "C", 0.02f}});
      CHECK(!hasEvent(ev, VoiceSelector::EventType::AutoSelected));
    }
    auto ev = tickVoice(s, t, {{"A", "A", 0.0f}, {"B", "B", 0.0f}, {"C", "C", 0.02f}});
    CHECK(hasEvent(ev, VoiceSelector::EventType::AutoSelected, "C"));
  });

  group("voice-selector: weak voice after 50 ticks, recovery, noDevice once", [] {
    VoiceSelector s;
    s.setCurrent("A");
    for (int i = 1; i <= 49; ++i) {
      auto ev = tickVoice(s, i * 200, {{"A", "A", 0.004f}, {"B", "B", 0.003f}});
      CHECK(!hasEvent(ev, VoiceSelector::EventType::WeakVoice));
    }
    auto ev = tickVoice(s, 50 * 200, {{"A", "A", 0.004f}, {"B", "B", 0.003f}});
    CHECK(hasEvent(ev, VoiceSelector::EventType::WeakVoice));
    CHECK(s.weak());
    for (int i = 51; i <= 70; ++i) {
      ev = tickVoice(s, i * 200, {{"A", "A", 0.004f}, {"B", "B", 0.003f}});
      CHECK(!hasEvent(ev, VoiceSelector::EventType::WeakVoice));
    }
    ev = tickVoice(s, 71 * 200, {{"A", "A", 0.01f}, {"B", "B", 0.003f}});
    CHECK(hasEvent(ev, VoiceSelector::EventType::WeakVoiceRecovered, "A"));
    CHECK(!s.weak());

    VoiceSelector n;
    CHECK(hasEvent(tickVoice(n, 0, {}), VoiceSelector::EventType::NoDevice));
    CHECK(tickVoice(n, 200, {}).empty());
    tickVoice(n, 400, {{"A", "A", 0.0f}});
    CHECK(hasEvent(tickVoice(n, 600, {}), VoiceSelector::EventType::NoDevice));
  });

  group("speaker-selector: Windows peak rule", [] {
    SpeakerSelector s;
    s.setCurrent("A");
    SpeakerLevel target;
    float cur = 0;
    for (int i = 0; i < 5; ++i) CHECK(!s.evaluate({{"A", "A", 0.5f}, {"B", "B", 0.9f}}, target, cur));
    CHECK(!s.evaluate({{"A", "A", 0.0f}, {"B", "B", 0.02f}}, target, cur));  // <= 0.02 never
    CHECK(!s.evaluate({{"A", "A", 0.0f}, {"B", "B", 0.02f}}, target, cur));
    CHECK(!s.evaluate({{"A", "A", 0.0f}, {"B", "B", 0.03f}}, target, cur));
    CHECK_EQ(s.pending(), std::string("B"));
    CHECK_EQ(s.consecutive(), 1);
    CHECK(s.evaluate({{"A", "A", 0.0f}, {"B", "B", 0.03f}}, target, cur));
    CHECK_EQ(target.uid, std::string("B"));
    CHECK_EQ(s.current(), std::string("B"));

    SpeakerSelector p;
    p.setCurrent("A");
    CHECK(!p.evaluate({{"A", "A", 0.0f}, {"B", "B", 0.03f}, {"C", "C", 0.0f}}, target, cur));
    CHECK(!p.evaluate({{"A", "A", 0.0f}, {"B", "B", 0.0f}, {"C", "C", 0.05f}}, target, cur));
    CHECK(p.evaluate({{"A", "A", 0.0f}, {"B", "B", 0.0f}, {"C", "C", 0.05f}}, target, cur));
    CHECK_EQ(target.uid, std::string("C"));

    SpeakerSelector q;
    q.setCurrent("A");
    CHECK(!q.evaluate({{"A", "A", 0.011f}, {"B", "B", 0.5f}}, target, cur));
    CHECK(!q.evaluate({{"A", "A", 0.011f}, {"B", "B", 0.5f}}, target, cur));
    CHECK(!q.evaluate({{"A", "A", 0.01f}, {"B", "B", 0.5f}}, target, cur));
    CHECK(q.evaluate({{"A", "A", 0.01f}, {"B", "B", 0.5f}}, target, cur));
    CHECK(std::fabs(cur - 0.01f) < 1e-6f);
  });

  group("meeting-state: native app active at once, 3 s inactive grace", [] {
    MeetingStateMachine m;
    MeetingOutput out;
    m.evaluate(0, {obs("zoom", false, false, false, {1})}, false, out);
    CHECK(hasLog(out, "ACTIVE zoom"));
    CHECK_EQ(out.notifies.size(), size_t(1));
    CHECK(out.notifies[0].isActive);
    CHECK_EQ(prop(out.notifies[0].meta, "isActive"), std::string("1"));
    CHECK_EQ(prop(out.notifies[0].meta, "appName"), std::string("zoom"));
    out = MeetingOutput();
    m.evaluate(100, {}, false, out);
    CHECK(hasLog(out, "INACTIVE pending (start) zoom"));
    CHECK_EQ(prop(out.logs[0].meta, "delaySeconds"), std::string("3"));
    CHECK(out.notifies.empty());
    CHECK_EQ(m.nextDeadline(), int64_t(3100));
    out = MeetingOutput();
    m.evaluate(1000, {}, true, out);
    CHECK(hasLog(out, "INACTIVE pending (keep) zoom"));
    out = MeetingOutput();
    m.evaluate(3099, {}, false, out);
    CHECK(out.notifies.empty());
    out = MeetingOutput();
    m.evaluate(3100, {}, false, out);
    CHECK(hasLog(out, "INACTIVE zoom"));
    CHECK_EQ(out.notifies.size(), size_t(1));
    CHECK(!out.notifies[0].isActive);
    CHECK_EQ(prop(out.notifies[0].meta, "isActive"), std::string("0"));
    CHECK(!m.isActive("zoom"));
    CHECK_EQ(m.nextDeadline(), int64_t(-1));
  });

  group("meeting-state: cancel inactive when capture resumes", [] {
    MeetingStateMachine m;
    MeetingOutput out;
    m.evaluate(0, {obs("slack", false, false, false, {7})}, false, out);
    m.evaluate(100, {}, false, out);
    out = MeetingOutput();
    m.evaluate(2000, {obs("slack", false, false, false, {7})}, false, out);
    CHECK(hasLog(out, "CANCEL INACTIVE slack"));
    CHECK(out.notifies.empty());
    CHECK(m.isActive("slack"));
  });

  group("meeting-state: browser needs 2.5 s of stable WebRTC evidence", [] {
    MeetingStateMachine m;
    MeetingOutput out;
    m.evaluate(0, {obs("chrome", true, false, false, {5})}, false, out);
    CHECK(out.logs.empty());
    CHECK(out.notifies.empty());
    m.evaluate(100, {obs("chrome", true, true, false, {5})}, false, out);
    CHECK(hasLog(out, "ACTIVE pending (start) chrome"));
    CHECK_EQ(prop(out.logs[0].meta, "stableMs"), std::string("2500"));
    CHECK_EQ(m.nextDeadline(), int64_t(2600));
    out = MeetingOutput();
    m.evaluate(2599, {obs("chrome", true, true, false, {5})}, false, out);
    CHECK(out.notifies.empty());
    m.evaluate(2600, {obs("chrome", true, true, false, {5})}, false, out);
    CHECK(hasLog(out, "ACTIVE chrome"));
    CHECK_EQ(out.notifies.size(), size_t(1));

    MeetingStateMachine c;
    out = MeetingOutput();
    c.evaluate(0, {obs("firefox", true, true, false, {9})}, false, out);
    out = MeetingOutput();
    c.evaluate(1000, {obs("firefox", true, false, false, {9})}, false, out);
    CHECK(hasLog(out, "ACTIVE pending (cancel) firefox"));
    c.evaluate(5000, {obs("firefox", true, false, false, {9})}, false, out);
    CHECK(out.notifies.empty());
  });

  group("meeting-state: title-only session lasts while capturing; UDP session ends with UDP", [] {
    MeetingStateMachine m;
    MeetingOutput out;
    m.evaluate(0, {obs("chrome", true, false, true, {5})}, false, out);
    m.evaluate(2500, {obs("chrome", true, false, true, {5})}, false, out);
    CHECK(m.isActive("chrome"));
    out = MeetingOutput();
    m.evaluate(3000, {obs("chrome", true, false, false, {5})}, false, out);
    CHECK(out.logs.empty());
    CHECK(m.isActive("chrome"));
    m.evaluate(4000, {}, false, out);
    CHECK(hasLog(out, "INACTIVE pending (start) chrome"));
    out = MeetingOutput();
    m.evaluate(7000, {}, false, out);
    CHECK_EQ(out.notifies.size(), size_t(1));

    MeetingStateMachine u;
    out = MeetingOutput();
    u.evaluate(0, {obs("chrome", true, true, false, {5})}, false, out);
    u.evaluate(2500, {obs("chrome", true, true, false, {5})}, false, out);
    CHECK(u.isActive("chrome"));
    out = MeetingOutput();
    u.evaluate(3000, {obs("chrome", true, false, false, {5})}, false, out);
    CHECK(hasLog(out, "INACTIVE pending (start) chrome"));
    out = MeetingOutput();
    u.evaluate(6000, {obs("chrome", true, false, false, {5})}, false, out);
    CHECK(hasLog(out, "INACTIVE chrome"));
    CHECK_EQ(out.notifies.size(), size_t(1));
  });

  group("meeting-state: stream turnover is a new session (2 s throttle)", [] {
    MeetingStateMachine m;
    MeetingOutput out;
    m.evaluate(0, {obs("zoom", false, false, false, {1})}, false, out);
    out = MeetingOutput();
    m.evaluate(1000, {obs("zoom", false, false, false, {2})}, false, out);
    CHECK(hasLog(out, "COUNTER RESET -> NEW SESSION zoom"));
    CHECK(out.notifies.empty());
    out = MeetingOutput();
    m.evaluate(1500, {obs("zoom", false, false, false, {2, 3})}, false, out);
    CHECK(out.logs.empty());
    out = MeetingOutput();
    m.evaluate(5000, {obs("zoom", false, false, false, {4})}, false, out);
    CHECK(hasLog(out, "COUNTER RESET -> NEW SESSION zoom"));
    CHECK(hasLog(out, "ACTIVE zoom"));
    CHECK_EQ(out.notifies.size(), size_t(1));
    out = MeetingOutput();
    m.evaluate(6000, {obs("zoom", false, false, false, {4}), obs("discord", false, false, false, {8})}, false, out);
    CHECK_EQ(out.notifies.size(), size_t(1));
    CHECK_EQ(out.notifies[0].appName, std::string("discord"));
  });

  group("app-catalog: lookups and JS knownAppNames parity", [] {
    CHECK(catalogByBinary("zoom") != nullptr && catalogByBinary("zoom")->appName == "zoom");
    CHECK(catalogByBinary("ZoomLauncher") != nullptr && catalogByBinary("ZoomLauncher")->appName == "zoom");
    CHECK(catalogByBinary("teams-for-linux") != nullptr && catalogByBinary("teams-for-linux")->appName == "ms-teams");
    CHECK(catalogByBinary("google-chrome-stable") != nullptr &&
          catalogByBinary("google-chrome-stable")->appName == "chrome");
    CHECK(catalogByBinary("firefox") != nullptr && catalogByBinary("firefox")->engine == BrowserEngine::Gecko);
    CHECK(catalogByBinary("bash") == nullptr);
    CHECK(catalogBySandboxId("us.zoom.Zoom") != nullptr && catalogBySandboxId("us.zoom.Zoom")->appName == "zoom");
    CHECK(catalogBySandboxId("com.google.Chrome") != nullptr &&
          catalogBySandboxId("com.google.Chrome")->appName == "chrome");
    CHECK(catalogByAppName("ZOOM VoiceEngine") != nullptr && catalogByAppName("ZOOM VoiceEngine")->appName == "zoom");
    CHECK(catalogByAppName("Google Chrome") != nullptr && catalogByAppName("Google Chrome")->appName == "chrome");
    CHECK(isAudioDaemon("pipewire-pulse"));
    CHECK(isAudioDaemon("wireplumber"));
    CHECK(isGenericRuntime("electron"));
    // Every reported appName must resolve in the app's JS matcher, and every
    // browser must be a browser for isBrowserApp().
    for (const CatalogEntry& e : appCatalog()) {
      const std::string real = jsRealName(e.appName);
      if (real.empty()) fail(__FILE__, __LINE__, "appName not matched by knownAppNames: " + e.appName);
      if (e.isBrowser() && !jsIsBrowser(real)) fail(__FILE__, __LINE__, "browser not a JS browser: " + e.appName);
      if (!e.isBrowser() && jsIsBrowser(real)) fail(__FILE__, __LINE__, "native app looks like a browser: " + e.appName);
      ++gChecks;
    }
  });

  group("proc-util: cgroup parsing", [] {
    CHECK_EQ(flatpakIdFromCgroup("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                                 "app-flatpak-us.zoom.Zoom-12345.scope"),
             std::string("us.zoom.Zoom"));
    CHECK_EQ(snapNameFromCgroup("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                                "snap.zoom-client.zoom-client-0f1e2d3c-1111-2222-3333-444455556666.scope"),
             std::string("zoom-client"));
    CHECK_EQ(desktopIdFromCgroup("0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                                 "app-gnome-org.mozilla.firefox-4242.scope"),
             std::string("org.mozilla.firefox"));
    CHECK_EQ(flatpakIdFromCgroup("0::/user.slice/user-1000.slice/session-2.scope"), std::string());
  });

  group("webrtc-evidence: /proc/net/udp parsing and classification", [] {
    UdpSocketEntry e;
    CHECK(parseProcNetUdpLine("  123: 00000000:C350 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000"
                              "        0 4242 2 0000000000000000 0",
                              false, e));
    CHECK_EQ(int(e.localPort), 50000);
    CHECK_EQ(e.inode, uint64_t(4242));
    CHECK(classifyUdpSocket(e) == UdpVerdict::Accepted);
    CHECK(parseProcNetUdpLine("  124: 0100007F:C351 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000"
                              "        0 4243 2 0000000000000000 0",
                              false, e));
    CHECK(classifyUdpSocket(e) == UdpVerdict::Loopback);
    CHECK_EQ(formatUdpEndpoint(e), std::string("127.0.0.1:50001"));
    CHECK(parseProcNetUdpLine("  125: FB0000E0:14E9 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000"
                              "        0 4244 2 0000000000000000 0",
                              false, e));
    CHECK(classifyUdpSocket(e) == UdpVerdict::Multicast);
    CHECK(parseProcNetUdpLine("  126: 0A00000A:C352 08080808:01BB 01 00000000:00000000 00:00000000 00000000  1000"
                              "        0 4245 2 0000000000000000 0",
                              false, e));
    CHECK(classifyUdpSocket(e) == UdpVerdict::Connected);
    CHECK(parseProcNetUdpLine("  127: 00000000:14E9 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000"
                              "        0 4246 2 0000000000000000 0",
                              false, e));
    CHECK(classifyUdpSocket(e) == UdpVerdict::ReservedPort);
    CHECK(parseProcNetUdpLine("   10: 00000000000000000000000000000000:C353 00000000000000000000000000000000:0000 07 "
                              "00000000:00000000 00:00000000 00000000  1000        0 4247 2 0000000000000000 0",
                              true, e));
    CHECK(e.v6);
    CHECK(classifyUdpSocket(e) == UdpVerdict::Accepted);
    CHECK(parseProcNetUdpLine("   11: 00000000000000000000000001000000:C354 00000000000000000000000000000000:0000 07 "
                              "00000000:00000000 00:00000000 00000000  1000        0 4248 2 0000000000000000 0",
                              true, e));
    CHECK(classifyUdpSocket(e) == UdpVerdict::Loopback);
    CHECK(!parseProcNetUdpLine("  sl  local_address rem_address   st tx_queue rx_queue", false, e));
  });

  group("window-titles: meeting services", [] {
    CHECK_EQ(meetingServiceFromTitle("Meet - abc-defg-hij - Google Chrome"), std::string("google-meet"));
    CHECK_EQ(meetingServiceFromTitle("Weekly sync | Microsoft Teams - Chromium"), std::string("teams"));
    CHECK_EQ(meetingServiceFromTitle("Zoom Meeting - Mozilla Firefox"), std::string("zoom"));
    CHECK_EQ(meetingServiceFromTitle("Huddle: #infra - Slack - Google Chrome"), std::string("slack"));
    CHECK_EQ(meetingServiceFromTitle("Jitsi Meet - Mozilla Firefox"), std::string("jitsi"));
    CHECK_EQ(meetingServiceFromTitle("Inbox (3) - Gmail - Google Chrome"), std::string());
    CHECK_EQ(meetingServiceFromTitle("Meeting notes.md - Visual Studio Code"), std::string());
  });

  group("json: parser", [] {
    JsonValue v;
    std::string err;
    CHECK(parseJson("{\"name\":\"alsa_input.usb-1\",\"n\":[1,2.5,-3e2],\"t\":true,\"z\":null,\"u\":\"\\u00e9\\u4e2d\"}",
                    v, &err));
    CHECK_EQ(v.stringOr("name"), std::string("alsa_input.usb-1"));
    CHECK_EQ(v["n"].items().size(), size_t(3));
    CHECK(std::fabs(v["n"].items()[2].asNumber() + 300.0) < 1e-9);
    CHECK(v["t"].asBool());
    CHECK(v["z"].isNull());
    CHECK_EQ(v.stringOr("u"), std::string("\xC3\xA9\xE4\xB8\xAD"));
    CHECK(!parseJson("{\"a\":}", v, &err));
    CHECK(!parseJson("[1,2", v, &err));
    CHECK(parseJson("[{\"pid\":42,\"class\":\"google-chrome\",\"title\":\"Meet - x\"}]", v, &err));
    CHECK_EQ(v.items()[0].intOr("pid", 0), int64_t(42));
  });

  group("level-meter: voice band filter and smoothing", [] {
    const double kPi = 3.14159265358979323846;
    auto run = [&](double freq, double amp, int buffers, VoiceLevelMeter& m, int64_t& n) {
      std::vector<float> buf(1024);
      for (int b = 0; b < buffers; ++b) {
        for (size_t i = 0; i < buf.size(); ++i, ++n) buf[i] = static_cast<float>(amp * std::sin(2 * kPi * freq * n / 48000.0));
        m.process(buf.data(), buf.size());
      }
    };
    VoiceLevelMeter m1;
    int64_t n = 0;
    run(1000, 0.5, 1, m1, n);
    CHECK(m1.level() > 0.12f && m1.level() < 0.15f);  // attack 0.4 of ~0.34
    run(1000, 0.5, 92, m1, n);
    CHECK(m1.level() > 0.33f && m1.level() < 0.36f);
    const float before = m1.level();
    run(1000, 0.0, 1, m1, n);
    CHECK(m1.level() < before && m1.level() > before * 0.9f);  // release 0.05
    VoiceLevelMeter m2;
    n = 0;
    run(50, 0.5, 93, m2, n);
    CHECK(m2.level() > 0.035f && m2.level() < 0.06f);  // 50 Hz is outside the voice band
  });

  group("device-filter: physical devices only", [] {
    DeviceNode mic;
    mic.name = "alsa_input.usb-Example_Vendor_USB_Microphone-00.mono-fallback";
    mic.description = "USB Microphone";
    mic.mediaClass = "Audio/Source";
    mic.props = {{"device.api", "alsa"}, {"device.bus", "usb"}};
    DeviceVerdict v = classifyDevice(mic, DeviceDirection::Capture);
    CHECK(v.allowed);
    CHECK_EQ(v.transport, std::string("usb"));
    DeviceNode virt = mic;
    virt.mediaClass = "Audio/Source/Virtual";
    CHECK(!classifyDevice(virt, DeviceDirection::Capture).allowed);
    DeviceNode filter = mic;
    filter.props.erase("device.api");
    CHECK(!classifyDevice(filter, DeviceDirection::Capture).allowed);
    DeviceNode named = mic;
    named.description = "Virtual Mic (Phone Link)";
    CHECK(!classifyDevice(named, DeviceDirection::Capture).allowed);
    DeviceNode flagged = mic;
    flagged.props["node.virtual"] = "true";
    CHECK(!classifyDevice(flagged, DeviceDirection::Capture).allowed);
    DeviceNode bt;
    bt.name = "bluez_output.AA_BB.1";
    bt.description = "Headset";
    bt.mediaClass = "Audio/Sink";
    bt.props = {{"device.api", "bluez5"}};
    CHECK(classifyDevice(bt, DeviceDirection::Render).allowed);
    CHECK(!classifyDevice(bt, DeviceDirection::Capture).allowed);
    CHECK_EQ(deviceDisplayName({{"node.nick", "Nick"}, {"node.name", "n"}}), std::string("Nick"));
  });

  std::printf("# %d checks, %d failures\n", gChecks, gTotalFailures);
  return gTotalFailures == 0 ? 0 : 1;
}
