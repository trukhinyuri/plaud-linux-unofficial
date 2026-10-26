<!--
Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
SPDX-License-Identifier: Apache-2.0
-->

# AudioMonitor on Linux: `audio_monitor_linux.node`

`native/audio-monitor/` is the Linux implementation of the native module that
Plaud Desktop loads as `audio_monitor_mac.node` on macOS and
`audio_monitor_win.node` on Windows. It exports one class, `AudioMonitor`,
which provides three features:

- **Meeting auto-detection.** It reports when a meeting application starts
  or stops using the microphone. The app uses these reports to prompt for,
  or automatically start, a recording.
- **Smart microphone.** While a recording runs with the microphone option
  "smart", it picks the physical microphone that is currently picking up
  voice.
- **Smart speaker.** While a recording captures system audio, it picks the
  physical output that is currently playing sound.

The module is written in C++17 against N-API (node-addon-api) and built with
cmake-js. Its only runtime dependency is `libpipewire-0.3`. libxcb is loaded
with `dlopen` when X11 window titles are needed.

## How to read this document

Each claim about Plaud's behaviour carries a tag:

- **[O] Observed.** The evidence is given: a file and line in the official
  JS bundle (`out-global-online/main/…`, byte-identical on macOS and
  Windows), a string or disassembly location in the official addons, or a
  log line from the real app on a Mac.
- **[D] Derived.** A design decision made for Linux, with the reasoning.
- **[?] Unknown.** Not established by the evidence available.

Addon paths: `mac` means `audio_monitor_mac.node` (arm64 slice) and `win`
means `win/app64/resources/audio_monitor_win.node`. A "strings" citation
means the literal is present in that binary. A disassembly citation gives
the function address in radare2 notation.

---

## 1. Loading and lifecycle

- **[O] Loading.** The JS loads the addon once, when the module is first
  evaluated:
  - The filename comes from `index$1.isMac ? "audio_monitor_mac.node" :
    "audio_monitor_win.node"` and the path from `monitorPrebuiltPath`
    (`createOrFocusWindow-DNFqWFOl.js:32-46`).
  - The JS uses `mod.AudioMonitor`.
  - The patcher points this loader at `audio_monitor_linux.node` on Linux
    (see the contract and `docs/patches.md`).
- **[O] One instance.** `getMeetingDetector()` creates a single instance
  lazily (`createOrFocusWindow-DNFqWFOl.js:175-182`). The meeting detector,
  smart microphone and smart speaker all go through that one object
  (`:208`, `:330`, `:340`, `:347`, `:357`).
- **[O] `stop()` is never called.** No file in `main/*.js` calls `stop()` on
  the detector.
  - **[D]** The addon therefore registers an N-API environment cleanup hook
    (`napi_add_env_cleanup_hook`). When Electron quits, the hook stops the
    detector, both device monitors and the PipeWire thread, and releases the
    JS callbacks. No thread outlives the JS environment.
- **[D] Threads.**
  - Constructing an `AudioMonitor` starts no thread and opens no connection.
    The test `api-surface.mjs` checks this.
  - All three features share one PipeWire connection, which runs on a
    `pw_thread_loop` named `plaud-am-pw`. It is reference-counted: it
    starts with the first feature and stops when the last one stops.
  - Meeting detection adds one worker thread while `start()` is active. The
    worker does the `/proc` scans and the compositor IPC, so the PipeWire
    loop never blocks on them.
  - `stop()` and the `stop*Monitor()` methods join their threads before
    returning. For `start()` and `stop()`, `api-surface.mjs` confirms this
    by counting the threads in `/proc/self/task`.

## 2. API surface

| JS call | Returns / throws | Linux implementation | Evidence | Test |
|---|---|---|---|---|
| `new AudioMonitor()` | instance | Allocates the three feature objects; no I/O | [O] `createOrFocusWindow-DNFqWFOl.js:180` | `api-surface.mjs`: "module exports…", "loading and constructing…" |
| `start(cb)` | `undefined`. Throws `TypeError("A callback function is required.")` if `cb` is not a function. Throws `Error("Monitor has already been started.")` if already running. | Opens a thread-safe function named `AudioMonitorCallback`, starts the detector worker, joins the shared PipeWire connection and logs `Starting PipeWire monitor v1.0.0` | [O] win strings `A callback function is required.`, `Monitor has already been started.`, `AudioMonitorCallback`; JS `:211` | `api-surface.mjs`: "callbacks are validated…", "start twice throws…"; all `meeting-*.test.mjs` |
| `stop()` | `undefined`. No-op when not running. | Logs `Stopping monitor`, stops the detector, leaves the PipeWire connection, joins threads | [O] mac and win strings `Stopping monitor` | `api-surface.mjs`: "stop methods are no-ops…", "start twice throws, stop releases the PipeWire thread" |
| `startMicMonitor(cb)` | `string`: the initially selected device uid, or `""`. Throws the TypeError above. | Thread-safe function `MicMonitorCallback`; starts the smart-microphone monitor (§5). If it is already running, the call replaces the callback and returns the current uid. | [O] JS `app-BMVDJmxZ.js:74`, `:103-105` (uses the uid when non-empty); win strings `MicMonitorCallback` | `mic-monitor.test.mjs` |
| `stopMicMonitor()` | `undefined` | Stops the meters and releases the callback | [O] JS `app-BMVDJmxZ.js:111` | `mic-monitor.test.mjs`, `api-surface.mjs` |
| `startSpeakerMonitor(cb)` | `string` uid or `""`. Throws the TypeError above. | Thread-safe function `SpeakerMonitorCallback`; starts the smart-speaker monitor (§6). A repeated call behaves as for the microphone. | [O] JS `app-BMVDJmxZ.js:134`, `:147-149`; win strings `SpeakerMonitorCallback` | `speaker-monitor.test.mjs` |
| `stopSpeakerMonitor()` | `undefined` | Stops the meters and releases the callback | [O] JS `app-BMVDJmxZ.js:155` | `speaker-monitor.test.mjs`, `api-surface.mjs` |

**[O] Exports.** The module exports exactly `{ AudioMonitor }`, the only
member the JS reads (`:45`).

**[D] Device uids.** A device uid is the PipeWire `node.name`. The Linux
Recorder addon uses the same uid in its device lists, so the uid in a
`micEvent` or `speakerEvent` can be passed to the recorder unchanged. This
was agreed with the Recorder slice.

## 3. Events

All values in `meta` objects are strings. **[O]** The mac addon builds meta
from `unordered_map<string,string>`; see the mangled `AudioMonitorMac::start`
symbol in the mac strings.

### 3.1 Meeting events (`start` callback)

```text
{ type: "monitor",
  obj: { notifyType: 1, appName: "<app>", isActive: true | false,
         meta: { appName, isActive: "1" | "0", processName, pid, deviceId, deviceName,
                 audioDirection: "input", evidence?, udpSocket?, meetingService?,
                 browserEngine?, flatpakAppId?, snapName? } } }
```

How the JS consumes these events:

- **[O] Required fields.** The JS drops objects whose `isActive` is not a
  boolean (`createOrFocusWindow-DNFqWFOl.js:231`).
- **[O] Ignored names.** It ignores app names that contain `plaud`
  (`:236`).
- **[O] Clear.** `notifyType === 2` clears all state (`:243-254`).
- **[O] Name mapping.** `appName` is matched case-insensitively by
  substring against `knownAppNames` (`:147-174`, `:262-282`).
- **[O] Batching.** Results are emitted after a 1 s batching window, and
  Webex instances are aggregated (`:283-323`).
- **[O] Meta.** Meta is only logged (`:235`).
- **[O] Field names on mac and win.** The mac strings include `type`,
  `monitor`, `appName`, `processName`, `isActive` and `notifyType`. The
  win strings include the same names plus `audioDirection`, `input`,
  `clientPid` and `clientProcessName`.

Linux behaviour:

- **[D] No notifyType 2.** Linux sends only `notifyType: 1`. Each session
  ends with its own `isActive: false`, so there is nothing to clear.
- **[?]** When the mac and win addons send `notifyType: 2` is not
  established.

The meta fields on Linux:

| Field | Meaning |
|---|---|
| `pid`, `processName` | The capturing process, like `clientPid` on win |
| `deviceId`, `deviceName` | The microphone node being captured |
| `evidence` | `webrtc-udp` or `window-title` (browsers only, §4.4) |
| `udpSocket` | `<pid>:<addr>:<port>` of the first accepted socket |
| `meetingService` | For example `google-meet`; set for title evidence |
| `browserEngine` | `chromium`, `gecko` or `webkit` |
| `flatpakAppId`, `snapName` | When the app is sandboxed |

### 3.2 Log events (all features, delivered through the `start` callback)

```text
{ type: "log", obj: { source: "NODE", level: "debug" | "info" | "warn" | "error", message, meta } }
```

- **[O] Shape and handling.** See `createOrFocusWindow-DNFqWFOl.js:212-229`.
  The JS collapses identical consecutive messages into "(repeated N times)"
  and writes them through its `native/monitor` logger. The mac strings
  include `source`, `level`, `message`, `meta` and `NODE`.
- **[D] Logs need `start()`.** The device monitors log through the same
  channel, so their logs reach the app only while `start()` is active, as
  with one callback per addon on mac and win.
- **[D] No stdout.** Nothing is written to stdout or stderr.

### 3.3 Microphone events (`startMicMonitor` callback)

```text
{ type: "micEvent", micEventType, deviceId, deviceName }
```

| `micEventType` | When | JS reaction [O] `app-BMVDJmxZ.js` |
|---|---|---|
| `deviceAutoSelected` | Voice is clearly stronger on another microphone (§5.3) | sets the smart mic (`:84-92`) |
| `deviceFallbackRequired` | The selected microphone disappeared, or after "no device" a microphone appeared | sets the smart mic (`:75-83`) |
| `weakVoice` (`deviceId`/`deviceName` empty) | No voice on any microphone for 10 s | logs a warning (`:93-94`) |
| `weakVoiceRecovered` | Voice again after `weakVoice` | logs (`:95-99`) |
| `noDevice` (empty id and name) | The last physical microphone disappeared | clears the smart mic (`:100-102`) |

**[O]** The event type literals appear in the win strings.

### 3.4 Speaker events (`startSpeakerMonitor` callback)

```text
{ type: "speakerEvent", speakerEventType: "speakerAutoSelected" | "noSpeaker", deviceId, deviceName }
```

**[O]** The JS sets or clears `smartSelectedSpeakerAtom` on these events
(`app-BMVDJmxZ.js:135-146`).

---

## 4. Meeting detection

### 4.1 What each platform observes

**[O] macOS** polls power assertions:

- Strings: `Starting pmset monitor (no daemon) v`, `1.0.5`,
  `/usr/bin/pmset -g assertions 2>/dev/null`,
  `AUDIO_MONITOR_POLL_INTERVAL_MS`.
- An audio assertion is mapped to the app that holds it. Real-app log on the
  Mac: `Audio PID N matched assertion: zoom.us (via PID N)`.
- Chromium additionally needs the `WebRTC has active PeerConnection`
  assertion (`ignored (Chromium without WebRTC PeerConnections)`).
- Firefox and WebKit need UDP evidence (`Firefox ignored (missing mandatory
  UDP evidence)`, `WebKit verified via sibling WebKit.Networking UDP`,
  `listUdpBindsForPid`).

**[O] Windows** enumerates WASAPI capture sessions per input endpoint:

- `IAudioSessionControl2` strings.
- `[win_audio_monitor]start monitorThread, version 0.0.12`.
- `audioDirection` = `input`, `clientPid`, `clientProcessName`.

**[D] Linux** uses the PipeWire graph, the direct analogue of WASAPI
capture sessions. The addon mirrors the registry: nodes, links, clients and
the `default` metadata. An application is *capturing the microphone* while
one of its nodes meets all of these conditions:

- `media.class` is `Stream/Input/Audio`;
- its state is `running` and it is not `pulse.corked`;
- it is not a peak meter (`stream.monitor` is not `true`);
- it is linked from a node whose class starts with `Audio/Source` or is
  `Audio/Duplex`. Physical microphones, virtual sources and echo-cancel
  sources all qualify.

The following do **not** count:

- Captures of an output or monitor (system audio): test "capture of an
  output (system audio), not a microphone, is not a meeting".
- The monitor's own process and its descendants: test "the monitor process
  and its descendants are never reported".
- Audio daemons.

### 4.2 From a stream to an application

The capturing process is resolved in this order (`src/app_identity.*`):

1. **[D] Host pid.**
   - Native PipeWire clients: the client's `pipewire.sec.pid`, set by the
     server from `SO_PEERCRED`.
   - pipewire-pulse clients: the node's `application.process.id`.
     - Test: "pulse client: identified from application.name and
       application.process.id".
   - Flatpak apps: that pid is in the sandbox namespace, so the host
     processes are found through the `app-flatpak-<id>-<n>.scope` cgroup.
   - Snap apps: identified by the `snap.<name>.<app>` cgroup.
2. **[D] Catalog lookup.** The first match wins, in this order:
   - the Flatpak id;
   - the Snap name;
   - the stream's `application.process.binary`, unless it is a generic
     runtime;
   - the stream's `application.name`, then its `application.id`;
   - the capturing process. In order:
     - its Flatpak id;
     - its Snap name;
     - its executable and `comm` (unless they are generic runtimes);
     - its XDG desktop id from the cgroup;
     - install path markers in its executable path;
     - for generic runtimes, path markers in argv. Generic runtimes are
       electron, node, bun, deno, python, java, mono, dotnet, sh, bash,
       wine and the sandbox launchers;
   - the PipeWire client's `application.process.binary` and
     `application.name`.
3. **[D] Process tree walk.** Helper processes belong to the application
   of their parent. The walk climbs while the parent:
   - has the same catalog entry;
   - runs the same non-generic executable;
   - is the main process of a system-Electron child (`--type=`); or
   - belongs to the same Flatpak.

   If nothing matched yet, the catalog is tried once more on the root
   process.

   Log: `Audio PID <stream pid> matched capture stream: <app> (via PID
   <root pid>)`. This follows the mac format (`Audio PID N matched
   assertion: X (via PID M)`); the mechanism name in the text differs.
4. **[D] Unknown applications.** When no catalog entry matches, the
   reported name is the first available of:
   - the stream's `application.process.binary`;
   - its `application.name`;
   - the executable name;
   - the client's `application.name`.

   **[O]** The JS blacklists any name that matches no `knownAppNames` key
   (`:279-281`), as it does on mac and win.

**Catalog.** The reported `appName` always contains the key the JS matches.

| Reported `appName` | JS key → app [O] `:147-174` | Linux identities (examples) |
|---|---|---|
| `zoom` | `zoom` | `zoom`, `zoom.real`, Flatpak `us.zoom.Zoom`, `/opt/zoom/` |
| `ms-teams` | `ms-teams` → `teams` | `teams-for-linux`, Flatpak `com.github.IsmaelMartinez.teams_for_linux`, `com.microsoft.Teams` |
| `slack` | `slack` | `slack`, `com.slack.Slack`, `/lib/slack/` |
| `webex` | `webex` | `CiscoCollabHost`, `webex`, `com.cisco.Webex`, `/opt/Webex/` |
| `discord` | `discord` | Discord (all channels), Vesktop, WebCord, Legcord, ArmCord |
| `lark` / `feishu` | `lark` / `feishu` → `lark` | `lark`, `bytedance-lark`, `feishu`, `bytedance-feishu` |
| `tencentmeeting` | → `tencent` | `wemeetapp`, `com.tencent.wemeet` |
| `dingmeeting` | → `dingding` | `dingtalk`, `com.alibabainc.dingtalk` |
| `welinkmeeting` | → `huawei` | `welink`, `com.huawei.welink` |
| `whatsapp`, `aircall`, `tuple` | same | WhatsApp for Linux, ZapZap, Aircall, Tuple |
| `chrome` | `chrome` | Google Chrome (all channels), `com.google.Chrome` |
| `chrome (chromium)`, `chrome (brave)`, `chrome (vivaldi)`, `chrome (opera)`, `chrome (yandex)` | `chrome` | Chromium, ungoogled-chromium, Thorium, Brave, Vivaldi, Opera, Yandex (packages, Flatpak, Snap) |
| `msedge` | `msedge` | Microsoft Edge |
| `firefox`, `firefox (librewolf)`, `firefox (zen)`, `firefox (floorp)`, `firefox (waterfox)`, `firefox (mullvad)` | `firefox` | Firefox (ESR, Developer Edition, Nightly) and its forks |
| `safari (epiphany)` | `safari` | GNOME Web (WebKitGTK) |

**[O] Auto-record keys.**
- The global build accepts auto-record only for `browser`, `zoom`, `teams`,
  `slack`, `webex` and `lark` (`compatible-OR3mc4zn.js:665-672`).
- The CN build adds `tencent`, `dingding` and `huawei` (`:673-682`).
- The filter is applied in `recordingService-Bn35UmTZ.js:16-20`.
- A browser counts when `isBrowserApp(appName)` is true. That function
  accepts only `chrome`, `firefox`, `arc`, `safari` and `msedge`
  (`compatible-OR3mc4zn.js:661-664`; mapping at
  `createOrFocusWindow-DNFqWFOl.js:189`).
- The Linux catalog has a parity check in the selftest, "app-catalog:
  lookups and JS knownAppNames parity". It confirms that every reported
  name maps to the intended JS key.

### 4.3 Browsers need WebRTC evidence

**[O]** A browser that captures the microphone is not necessarily in a call.
Dictation and voice search also capture. The mac addon therefore requires
WebRTC evidence (§4.1).

**[D] Primary evidence on Linux: an ICE/media UDP socket** in the browser's
process tree (`src/webrtc_evidence.*`).

- **Scan.** For each process of the application:
  - `/proc/<pid>/fd` socket inodes are matched against
    `/proc/<pid>/net/udp{,6}`;
  - the tables are read per network namespace, so sandboxed browsers
    work.
- **Accepted:** an *unconnected* UDP socket bound to an interface or wildcard
  address on an unprivileged port. This is how WebRTC ICE gathers
  candidates.
- **Rejected:**
  - loopback;
  - multicast;
  - connected sockets (QUIC, DNS);
  - ports below 1024;
  - SSDP 1900, WS-Discovery 3702, mDNS 5353 and LLMNR 5355.
- **Debug log.** The counters are logged:
  `Audio PID N ignored (browser without WebRTC evidence): <app>` with
  `udpSockets`, `rejConnected`, `rejLoopback`, `rejMulticast`,
  `rejReservedPort` and `unreadablePids`. **[O]** mac logs comparable
  counters (`udpSockets`, `udpPassed`, `rejLoopback`).

**[D] Secondary evidence: the title of a browser window** that belongs to
the same process tree and shows a meeting service.

- **Services:** Google Meet, Microsoft Teams, the Zoom web client, Webex,
  Jitsi Meet, Whereby, Lark/Feishu meetings, Tencent Meeting/VooV,
  GoTo Meeting, RingCentral Video, BlueJeans and Slack huddles.
- **Meta:** `evidence: "window-title"` and `meetingService`.
- **Providers,** chosen once per process and logged as
  `Window title provider: <name>`:
  1. `PLAUD_AUDIO_MONITOR_WINDOW_LIST`. A JSON file for diagnostics and
     compositors without an IPC (see §9).
  2. Hyprland IPC (`j/clients`).
  3. Sway/i3 IPC (`GET_TREE`).
  4. X11 EWMH (`_NET_CLIENT_LIST`, `_NET_WM_PID`, `_NET_WM_NAME`) through
     dlopen'ed libxcb.
- **Limitation.** GNOME Shell and KDE Plasma on Wayland give clients no
  window list. There, native Wayland browser windows are invisible and UDP
  evidence decides alone. XWayland windows remain visible through X11.
- **[D] Lifetime of a title-only session.** It stays active while the
  capture continues. A session that had UDP evidence ends when the UDP
  evidence disappears; the call is over even if a tab keeps the microphone.

Tests (`meeting-browser.test.mjs`, using a stand-in executable named
`chrome`, `test/native/fake_chrome.c`):

- ACTIVE after 2.5 s with UDP evidence;
- no meeting without evidence;
- title evidence.

### 4.4 State machine and timings

| Behaviour | macOS | Windows | Linux |
|---|---|---|---|
| Poll and evaluation | [O] poll (`AUDIO_MONITOR_POLL_INTERVAL_MS`; default [?]) | [O] session events | [D] 1 s tick, plus graph changes debounced by 100 ms |
| Native app becomes ACTIVE | [O] at the first matching poll (`ACTIVE <app>`) | [O] on an active capture session | [D] when the stream starts running (graph change, 100 ms debounce), at the latest on the next 1 s tick |
| Browser becomes ACTIVE | [O] after the evidence is stable for 2.5 s (`ACTIVE pending (start)`, `stableMs`, `2500`; `ACTIVE pending (cancel)`) | [?] | [D] same rule and log texts as mac; the test checks `stableMs` = `2500` and ≥ 2.4 s from `ACTIVE pending (start)` to ACTIVE |
| INACTIVE after the capture ends | [O] 3 s grace: `INACTIVE pending (start)` `{delaySeconds:"3"}`, `INACTIVE pending (keep)`, `INACTIVE` (Mac app logs) | [O] `app_inactive_marked`, `app_inactive_since_set` | [D] same as mac; the test checks 2.9-5.5 s after the capture stops |
| Capture resumes within the grace period | [O] `CANCEL INACTIVE <app>` | [O] `app_inactive_cleared_by_reactivation` | [D] same as mac |
| Every capture stream replaced while active | [O] `COUNTER RESET -> NEW SESSION <app>` | [O] `notify_throttle_ok` | [D] new session, re-notified ACTIVE, with a 2 s per-app re-notify throttle (value [?] on win) |

The cancel and turnover rules are tested by "re-joining within the grace
period cancels INACTIVE and is a new session".

### 4.5 Recovery

When the PipeWire connection is lost:

- the detector receives an empty capture snapshot;
- active applications go through the normal 3 s INACTIVE grace and are
  reported inactive;
- after reconnection, detection resumes from the new graph.

This is tested by `pipewire-restart.test.mjs` (§7).

---

## 5. Smart microphone

### 5.1 Eligible devices

- **[O] mac and win use physical devices only.**
  - mac: `isAllowedTransportType`, `Filtered input device:`.
  - win: `isAllowedCaptureDevice`, and in the win wide strings `virtual` and
    `phone link`.
- **[D] Linux eligibility.** A device is eligible when all of these hold:
  - `media.class` is exactly `Audio/Source`;
  - `device.api` is `alsa` or `bluez5`;
  - `node.virtual` is not `true`;
  - neither the name nor the description contains "virtual" or
    "phone link".
- **[D] What this excludes:**
  - monitors;
  - null sinks and loopbacks;
  - echo-cancel nodes;
  - network streams;
  - virtual devices.
- **[D] Logging.** A filtered device is logged once at debug level as
  `[MicMonitor] Filtered input device: <name> transport=<bus>`.

### 5.2 Initial device and fallback

- **[O] Initial device on mac and win.** Both start from the system default
  input: mac `getSystemDefaultInput`, win `pickInitialDevice`. They log
  `Initial device: <name> uid=<uid>`.
- **[D] Linux ordering.** The system default source (PipeWire `default`
  metadata) comes first. The other devices follow in creation order
  (`object.serial`, which unlike node ids is never reused). The first
  eligible device is the initial selection.
  - Test: the default B is chosen although A was plugged in first.
- **[O] Fallback when the selected device disappears.**
  - Logs: `[MicMonitor] Device lost, fallback to: <name>` (mac) and
    `[MicMonitor] Device lost, fallback: ` (win).
  - Event: `deviceFallbackRequired`, for the first remaining device.
  - Test: "fallback to B".
- **[D] Last device removed.** `noDevice` is reported once, the selection is
  cleared, and `[MicMonitor] No physical input device available` is logged.
  **[O]** The win strings contain the same text.
- **[D] A device appears after `noDevice`.**
  - Event: `deviceFallbackRequired`.
  - Log: `[MicMonitor] Input device available, fallback to: <name>`.
  - Reason: the JS cleared its selection on `noDevice`
    (`app-BMVDJmxZ.js:100-102`), so it needs a new one.
- **[D] Unplug versus capture error.** When a meter fails, the failure is
  judged one 200 ms tick later:
  - if the device has left the graph, it was unplugged:
    `[MicMonitor] Device removed: <name>`;
  - otherwise it is a capture error:
    `[MicMonitor] Device died, capture stopped: <name>`. The device is
    retried after 10 s.
  - **[O]** mac strings: `Device removed: `, `Device died, IOProc stopped: `.

### 5.3 Voice detection and switching

Every eligible microphone is metered all the time:

- **[D] Capture.** A mono 48 kHz float capture stream targets the device
  node. It never moves to another device, which the stream props
  `node.dont-reconnect`, `node.dont-fallback` and `node.dont-move`
  guarantee.
- **[D] Meter streams.** They are marked `stream.monitor=true` and appear
  as `plaud-mic-meter`.
- **[O] Format log.** The Mac app logs
  `[MicMonitor] Capture started: <name> uid=<uid> fmt=<rate>Hz/<n>ch/32bit/float/interleaved`.
  Linux logs the same line with its fixed capture format,
  `fmt=48000Hz/1ch/32bit/float/interleaved`.

The level is computed per buffer:

- **[O] Voice-band filter.** A high-pass biquad (b = 0.9816582826,
  -1.9633165651, 0.9816582826; a1 = -1.9631912746, a2 = 0.9634418556)
  followed by a low-pass biquad (b = 0.0335593801, 0.0671187602,
  0.0335593801; a1 = -1.4180655436, a2 = 0.5523030641). Source: the
  decompiled mac microphone level computation. The win `.rdata` holds the
  same coefficients as doubles.
- **[O] Smoothing.** The RMS of each buffer is smoothed with an attack of
  0.4 when it rises and a release of 0.05 when it falls (same mac code).

Every 200 ms, the selector (`src/voice_selector.*`) evaluates all levels.
**[O]** The tick is mac `dispatch_source_set_timer(…, 200000000, …)`.

- **[O] Voice present.** Some device has a level ≥ 0.005.
- **[O] Weak voice.** After 50 consecutive ticks (10 s) without voice the
  selector sends `weakVoice` once and logs
  `[MicMonitor] Weak voice on ALL devices for 10s`. Evidence: the `0x32`
  tick counter in the mac and win selector. The next voice sends
  `weakVoiceRecovered` with the loudest device
  (`[MicMonitor] Voice recovered on <name>`).
- **[O] Switch candidate.** The loudest device, if it is not the current
  one, its level is ≥ 0.001, and the last switch was ≥ 3 s ago (win
  `fcn.180024130`: 3 000 000 000 ns).
- **[O] Win.** A tick counts as a "win" when
  `best / current ≥ 1.3 + 0.7 · clamp(current / 0.01, 0, 1)`; the ratio
  is 100 when current ≤ 1e-6. These are the win constants: .rdata
  `0x180060290` = 1.3, `0x180060280` = 0.7, `0x18006026c` = 0.01,
  `0x18006025c` = 1e-6, `0x1800602d0` = 100.0, used in `fcn.180024130`.
  - **[O]** mac uses `0.91 + clamp(…)` instead.
  - **[D]** Linux follows the non-mac path, as the contract requires.
- **[O] Switch.** Five wins inside a sliding 1.6 s window switch to the
  candidate: `deviceAutoSelected` and `[MicMonitor] Switch → <name>
  uid=<uid>`. The window length is 1 600 000 000 ns in both binaries.

Test (`mic-monitor.test.mjs`), in order:

1. initial device = the system default;
2. a tone on the other microphone switches to it;
3. unplugging it falls back;
4. 10 s of silence sends `weakVoice` (asserted ≥ 9.5 s);
5. a tone sends `weakVoiceRecovered`;
6. removing the last device sends `noDevice` once;
7. re-plugging sends `deviceFallbackRequired`;
8. `stopMicMonitor()` removes every meter stream.

---

## 6. Smart speaker

- **[O] Windows-only feature in the app.**
  - The JS never starts the speaker monitor on macOS:
    `if (index.isMac) return false;` (`app-BMVDJmxZ.js:124`).
  - mac answers `[SpeakerMonitor] macOS uses global tap, speaker monitor not
    needed`.
  - The JS runs it only while a recording with system audio is active
    (`:125-127`).
  - **[D]** Linux implements the Windows behaviour.
- **[O] Eligible outputs.** win checks `isAllowedRenderDevice`. **[D]** Linux
  applies the rule of §5.1 with `media.class` exactly `Audio/Sink`.
- **[D] Metering.** Each output is metered by a passive capture of its
  monitor ports. The stream is `plaud-speaker-meter` with
  `stream.capture.sink=true` and `node.passive=true`, so metering never
  keeps a sink running or audible. The value is the per-buffer peak.
  **[O]** win reads `IAudioMeterInformation::GetPeakValue`.
- **[O] Initial device.**
  - win logs `[SpeakerMonitor] Initial device by system default: <name>
    (peak=…)`. Otherwise it logs `Initial device by peak (default
    unavailable): …` or `Initial device by fallback: …`. Its decompiled
    initial-device code compares the best peak with 0.1 (`0x180060278`).
  - **[D]** Linux logs the system-default and fallback variants. See §10
    for "by peak".
- **[O] Switch rule** (win `0x18006026c` = 0.01, `0x180060270` = 0.02,
  `consecutive=`, `pendingSwitchUID=`):
  - the current output is kept while its peak is > 0.01;
  - otherwise the loudest other output must exceed 0.02 on 2 consecutive
    evaluations;
  - then `speakerAutoSelected` and `[SpeakerMonitor] Switch → <name>
    uid=<uid>, target peak=<p> (current peak=<p>)`.
  - **[D]** The Linux evaluation period is 500 ms, so a switch takes
    about 1 s. **[?]** The win period is not established.
- **[O] Device loss.**
  - When the selected output disappears: `[SpeakerMonitor] Device lost,
    fallback: <name>` and `speakerAutoSelected`.
  - When none is left: `[SpeakerMonitor] No render device available` and
    `noSpeaker`, once.
  - Rescans log `[SpeakerMonitor] Device change detected, rescanning`.
  - **[D]** After `noSpeaker`, the first new output is selected with
    `Device available, selecting: <name>`.
- **[O] Diagnostics.** win logs a `[SpeakerMonitor][diag-eval]` line every
  60 evaluations. In the decompiled evaluation, the counter is reset after
  exceeding 0x3b. **[D]** Linux does the same.

Test (`speaker-monitor.test.mjs`), in order:

1. the default sink is virtual, so the initial device comes "by fallback";
2. a tone into B switches to B;
3. a tone into A switches back;
4. A is kept while both play (no event within 2 s);
5. after a restart with B as the default, the initial device comes "by
   system default";
6. unplugging B falls back to A;
7. unplugging A sends `noSpeaker`.

---

## 7. PipeWire connection and recovery

- **[D] Startup.** It logs `Starting PipeWire connection` with
  `libraryVersion` and `headersVersion`, then `Connected to PipeWire`.
- **[D] Ready state.** The graph mirror counts as ready after two core
  roundtrips, so the initial device choice already sees every node and the
  default metadata.
- **[D] Connection loss** (daemon crash, `EPIPE`):
  - it logs `PipeWire connection lost, reconnecting` (warn);
  - it tears down every stream and proxy;
  - it reconnects with back-off from 0.5 s up to 5 s, then logs
    `Reconnected to PipeWire`.
- **[D] Device monitors during an outage.**
  - They pause their meters and send no events.
  - After reconnection they wait up to 3 s for the session manager to
    re-announce devices. WirePlumber re-creates ALSA nodes about a second
    after a restart.
  - During that wait they neither report "no device" nor fall back away
    from a selected device that may still return.
  - If the same devices come back, the JS sees no event at all.

Test (`pipewire-restart.test.mjs`), against a private daemon:

1. Zoom becomes ACTIVE.
2. SIGKILL the daemon: the loss is logged and INACTIVE follows after ≥ 3 s.
3. Restart the daemon: `Reconnected to PipeWire`.
4. Re-create the devices 1 s later: no microphone or speaker event, and
   the selections are unchanged.
5. A new Zoom capture becomes ACTIVE.
6. Unplugging the microphone falls back; unplugging the speaker sends
   `noSpeaker`.

Both reconnection grace periods were mutation-checked: with the grace set
to 0 the test fails.

## 8. Logging

- **[O] Mac log texts.** The message texts follow the mac addon wherever a
  mac equivalent exists. Examples: `ACTIVE`, `INACTIVE pending (start)`,
  `CANCEL INACTIVE`, `COUNTER RESET -> NEW SESSION`, `[MicMonitor] Switch →`
  and `Initial device:`. Support staff reading Linux logs therefore see
  familiar lines.
- **[D] Linux-only lines:**
  - the PipeWire connection lines (§7);
  - `Window title provider: …`;
  - `… matched capture stream …` and `… lost capture stream (no longer in
    meeting)`, the counterparts of mac's `… assertion …`;
  - `[MicMonitor] Input device available, fallback to: …`;
  - `[SpeakerMonitor] Device available, selecting: …`;
  - `… PipeWire connection lost, capture paused` and
    `… PipeWire connection lost, metering paused`.

## 9. Environment

| Variable | Effect |
|---|---|
| `PIPEWIRE_REMOTE`, `PIPEWIRE_RUNTIME_DIR`, `XDG_RUNTIME_DIR` | Standard libpipewire connection variables |
| `HYPRLAND_INSTANCE_SIGNATURE`, `SWAYSOCK`, `I3SOCK`, `DISPLAY` | Select the window title provider (§4.3) |
| `PLAUD_AUDIO_MONITOR_WINDOW_LIST=<file>` | Overrides the window title provider. The file is a JSON array of `{"pid":N,"appId":"…","title":"…"}` and is re-read on every query. Used by the tests; also lets a compositor bridge feed titles on desktops without a supported IPC. |

## 10. Deviations from macOS and Windows

| Deviation | Why |
|---|---|
| The mic switch ratio is win's `1.3 + 0.7·x`, not mac's `0.91 + x` | [D] The contract says to follow the non-mac path where they differ. |
| Chromium-family browsers (Chromium, Brave, Vivaldi, Opera, Yandex) are reported as `chrome (<variant>)`; Firefox forks as `firefox (<variant>)`; GNOME Web as `safari (epiphany)` | [D] On Linux, Chromium is the default browser on many distributions, and plain `chromium` matches no JS key. mac and win report Brave as `brave` (`knownAppNames` maps `brave` → `brave`, `:167`). `brave` is neither in `isBrowserApp` nor among the auto-record keys, so a Brave meeting is reported there but never acted on. With the Linux naming, the user's "browser" auto-record setting covers every installed browser. Side effect: the meeting prompt names the app "chrome", "firefox" or "safari". |
| All browsers need 2.5 s of stable evidence, and that evidence is UDP or a window title | [D] Linux has no `WebRTC has active PeerConnection` assertion. UDP is the evidence mac already uses for Firefox and WebKit. |
| No "Initial device by peak (default unavailable)" for the speaker | [D] PipeWire meters need a few buffers before reporting a peak, so the value is not available when `startSpeakerMonitor()` returns. The regular switch rule selects a playing output within about 1 s instead. |
| `noDevice` clears the selection; a device that appears afterwards is announced with `deviceFallbackRequired` | [D] It mirrors the JS, which clears the smart mic on `noDevice` (`app-BMVDJmxZ.js:100-102`). [?] What mac and win send when a device returns after `noDevice` is not established. |
| A 3 s reconnection grace for the device monitors | [D] An audio server restart is a normal Linux event (package updates, user restarts); CoreAudio and WASAPI have no equivalent client-visible restart. |

## 11. Build, dependencies, tests

- **Build.** `npm install && npm run build` (cmake-js, `CMakeLists.txt`)
  produces:
  - `build/Release/audio_monitor_linux.node`;
  - `build/Release/audio_monitor_selftest` (pure-logic tests);
  - `build/Release/test-bin/chrome` (the browser stand-in for the tests).
- **Linking.** The addon is compiled with hidden visibility and linked with
  `--as-needed` and `--exclude-libs,ALL`. `libpipewire-0.3` is the only
  `DT_NEEDED` library outside the C/C++ toolchain runtime (libc, libm,
  libstdc++, libgcc_s and the dynamic loader).
- **Portability.** Plain C++17 and C99, no architecture-specific code. It is
  built and tested on aarch64 (Arch Linux, PipeWire 1.6.9).
- **`npm test`** runs:
  - `api-surface.mjs`: every member, argument validation, the thread
    lifecycle and the log shape;
  - `selftest.test.mjs`: 18 groups. They cover the selectors, the state
    machine, catalog and JS parity, cgroups, `/proc/net/udp` parsing,
    window titles, JSON, the level meter and the device filter;
  - the integration tests: `meeting-native`, `meeting-browser`,
    `mic-monitor`, `speaker-monitor` and `pipewire-restart`.
- **Isolation of the integration tests.**
  - Each file starts a private PipeWire, WirePlumber (hardware monitors
    disabled) and pipewire-pulse instance in a temporary runtime, config
    and state directory, with `setpriv --pdeathsig`.
  - The restart test uses a minimal private daemon.
  - The tests therefore never open the machine's microphones or outputs,
    never change its defaults and leave nothing in its WirePlumber state.
  - The addon runs in a separate host process (`test/fixtures/monitor-host.mjs`),
    so the processes the tests spawn are not its descendants.
  - The integration tests are skipped with a reason when the PipeWire tools
    or the built addon are missing.

## 12. Open questions

- **[?] notifyType 2.** When mac and win send it.
- **[?] Windows timing values:**
  - the speaker evaluation period;
  - the value of the per-app re-notify throttle.
- **[?] Browser calls without UDP.** A WebRTC call that runs entirely over a
  TCP/TLS TURN relay opens no UDP socket. On GNOME and KDE Wayland, where
  no window titles are available, such a browser call is not detected.
  macOS's Chromium PeerConnection assertion does not depend on the
  transport.
