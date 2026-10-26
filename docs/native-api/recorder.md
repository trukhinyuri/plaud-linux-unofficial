<!--
Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
SPDX-License-Identifier: Apache-2.0
-->

# Recorder on Linux: `recorder_linux.node`

`native/recorder/` is the Linux implementation of the native module that
Plaud Desktop loads as `recorder_mac.node` on macOS and `recorder.node` on
Windows. It exports one class, `Recorder`, which provides:

- **Recording.** It captures the microphone and the system audio (what an
  output device plays), runs echo cancellation, noise suppression and gain
  control (WebRTC Audio Processing), mixes both sources and encodes Ogg Opus
  in the container layout of Plaud's own recorders.
- **Devices.** It lists microphones and outputs, watches microphones being
  plugged and unplugged, and follows the mute state of the default output.
- **Permissions.** It reports the system-audio and screen-capture
  permissions.
- **Screenshots.** It captures displays, windows and rectangles as PNG.
- **Offline AEC metrics.** `computeAecLightMetrics`, a pure function.

The module is C++17 against N-API (node-addon-api, `NAPI_VERSION` 8), built
with cmake-js. Audio goes through PipeWire only; a PulseAudio daemon is not
needed. WebRTC Audio Processing 2.1 is vendored and linked statically.
libX11 and libXrandr are loaded with `dlopen` when an X11 display is used.

## How to read this document

Each claim about Plaud's behaviour carries a tag:

- **[O] Observed.** The evidence is given:
  - a file and line in the official JS bundle. Paths are relative to
    `out-global-online/main/`; the bundle is the same on macOS and Windows;
  - `mac strings N`: line N of `strings -a -n 3 recorder_mac.node` (GNU
    binutils) on the universal binary; the arm64 slice comes first, so
    each literal appears twice and the first line is cited. `win strings
    N` is the same command on `win/app64/resources/recorder.node`;
  - a disassembly address (`@0x…`) in the arm64 slice of
    `recorder_mac.node`, as printed by `llvm-objdump -d` on that slice;
  - `oracle`: a call made against the official `recorder_mac.node` on a Mac
    with Node 26. The results are the expected values in
    `native/recorder/test/validation.test.mjs`, `test/aec-metrics.test.mjs`
    (`test/fixtures/aec-metrics/expected.json`) and `test/screen.test.mjs`;
  - `mac file`: an Ogg file written by the official app on macOS.
- **[D] Derived.** A design decision made for Linux, with the reasoning.
- **[?] Unknown.** Not established by the evidence available.

---

## 1. Loading, threads and lifecycle

- **[O] Loading.** The JS loads the addon once, when its module is first
  evaluated. It picks `index.isMac ? "recorder_mac.node" : "recorder.node"`,
  resolves it with `recorderPrebuiltPath`, `require`s it and uses
  `mod.Recorder` (`nativeRecorder-D-_1ie7u.js:49-67`). The patcher points
  this loader at `recorder_linux.node` (`docs/patches.md`,
  `recorder-addon-name`).
- **[O] Plaud uses webrtc-audio-processing 2.1.** The mac addon links
  `@rpath/libwebrtc-audio-processing-2.1.dylib` (mac strings 48) and
  imports `webrtc::AudioProcessingBuilder` from it (mac strings
  12130-12132). The Windows build ships
  `resources/webrtc-audio-processing-2-1.dll`, and the loader diagnoses that
  sidecar on a load failure (`nativeRecorder-D-_1ie7u.js:55-58`). Linux
  vendors the same library version (§7.4).
- **[O] Class surface.** The module exports only `Recorder`. The prototype
  has, in this order, `start, begin, pause, stop, restart, setAudioDevice,
  setApmEnabled, fillSilence, setEncodingPaused`. The statics, in order,
  are `checkSystemAudioPermission, requestSystemAudioPermission,
  checkScreenCapturePermission, checkScreenCapturePermissionFresh,
  requestScreenCapturePermission, captureScreenImage, getWindowAtCursor,
  extractTextFromImage, listMicrophoneDevices, listMicrophoneDevicesDetailed,
  watchMicrophoneDevices, stopWatchingMicrophoneDevices, listSpeakerDevices,
  listSpeakerDevicesDetailed, computeAecLightMetrics, startSpeakerMuteMonitor,
  stopSpeakerMuteMonitor, isSpeakerMuted`. Every function has
  `length === 0` [O oracle: the official class read with Node].
  `Recorder()` without `new` throws node-addon-api's class-constructor
  TypeError [D]. `test/api-surface.mjs` asserts all of this.
- **[O] One recorder per recording.** `RecordingService` keeps one
  `Recorder` and calls `start`, `pause`/`begin`, `setEncodingPaused`,
  `fillSilence`, `setAudioDevice`, `restart` and `stop` on it
  (`recordingService-Bn35UmTZ.js:267-433`, `:517`, `:618`).
- **[D] Threads.**
  - Constructing a `Recorder` starts no thread and opens no connection.
  - `start()` opens a PipeWire connection on a `pw_thread_loop` named
    `plaud-recorder-pw` and starts one worker thread, `plaud-rec-work`. The
    PipeWire `process` callbacks only copy samples into lock-free ring
    buffers (2.7 s each). The worker does everything else: 10 ms chunking,
    WebRTC APM, mixing and Opus encoding. It is also the only consumer of
    the rings (§7.2).
  - Events reach JS through one thread-safe function. Native threads push
    into a queue, and the JS thread drains it in order (`EventPump`).
  - `stop()` and `restart()` run synchronously on the JS thread.
  - The device statics share a second, lazily started connection
    (`DeviceHub`) with the microphone watcher and the mute monitor. When
    neither is running, each list call makes a short-lived connection
    (`plaud-query-pw`, 1.5 s connect and round-trip timeouts).
- **[D] Teardown without JS.** If Electron quits while a recording runs, an
  N-API environment cleanup hook stops the engine without calling into JS.
  A `Recorder` that is garbage-collected while recording is handled the
  same way. No thread outlives the JS environment.

## 2. API surface

Plaud usage: where the official JS calls the member. "Not used" means no
call exists anywhere under `out-global-online/` [O grep]. The member exists
for parity.

| JS call | Plaud usage [O] | Returns / throws | Linux implementation | Test |
|---|---|---|---|---|
| `new Recorder()` | `recordingService-Bn35UmTZ.js` | instance | No I/O | `api-surface.mjs` |
| `start(options, cb)` | `:267`, options `:248-256` | `{sampleRate: 48000, channels: 1, container: 'ogg'\|'pcm'}`. Throws on bad arguments and start failures (§4) | PipeWire connection, two capture streams in one `node.group`, APM, Opus encoder (§4-§8) | `validation`, `record`, `pause`, `default-switch`, `hotplug`, `aec`, `pipewire-restart` |
| `pause([mode])` | `:297-300`, `:321`, `:339`, `:406`, `:415` | `undefined` | Flag-only: the side stays linked but its samples are dropped (§5) | `validation` "methods on an idle recorder", `pause` |
| `begin([mode])` | `:319`, `:337`, `:404`, `:413` | `undefined` | Re-enables the side. A side not captured since start gets its stream added (§5) | `pause` |
| `stop()` | `:366` | `true` when it stopped a recording, else `undefined` | Drains, finishes the Ogg stream and delivers the last events before returning (§5) | `record`, every recording test |
| `restart()` | `:517`, inside the fatal callback | `true`/`false` | Rebuilds the PipeWire connection and streams. Retries for 3 s, then stops the recorder and returns `false` (§10) | `pipewire-restart` (3 tests) |
| `setAudioDevice(sel)` | `:433` | Echo of the selection (§6.4) | Re-targets the running streams | `validation`, `default-switch` |
| `setApmEnabled(on)` | Not used | `undefined` | Bypasses or re-enables APM on the running recorder | `validation`, `pause` |
| `fillSilence(ms)` | `:618` | `undefined` | Encodes `floor(ms/10)` 10 ms chunks of silence, capped at 24 h | `validation`, `pause` |
| `setEncodingPaused(on)` | `:358`, `:391`, `:397` | `undefined` | Capture and events continue; the encoder gets nothing | `validation`, `pause` |
| `Recorder.checkSystemAudioPermission(cb)` | `lifecycleService-GDc1zMWu.js:241` (macOS only, `:236-238`) | `0` Granted / `1` Denied | Granted when PipeWire is reachable (§9) | `validation`, `pipewire-restart` |
| `Recorder.requestSystemAudioPermission(cb)` | `lifecycleService-GDc1zMWu.js:274` (macOS only) | `true`/`false` | Same check | `validation`, `pipewire-restart` |
| `Recorder.listMicrophoneDevices()` | Not used | `string[]` of names | PipeWire `Audio/Source` nodes (§6.2) | `validation` |
| `Recorder.listMicrophoneDevicesDetailed()` | `recordingService-Bn35UmTZ.js:41`, `:52` | `{id, name, uid, isDefault}[]` | Same | `validation`, `hotplug`, `pipewire-restart` |
| `Recorder.watchMicrophoneDevices(cb)` | `recordingService-Bn35UmTZ.js:129` | `true` | Registry watcher, 150 ms debounce, `cb(names)` (§6.5) | `validation`, `hotplug`, `pipewire-restart` |
| `Recorder.stopWatchingMicrophoneDevices()` | Not used | `true` | Releases the callback | `validation`, `hotplug` |
| `Recorder.listSpeakerDevices()` | Not used | `string[]` | PipeWire `Audio/Sink` nodes | `validation` |
| `Recorder.listSpeakerDevicesDetailed()` | `recordingService-Bn35UmTZ.js:42` | `{id, name, uid, isDefault}[]` | Same | `validation`, `default-switch` |
| `Recorder.startSpeakerMuteMonitor(cb)` | Not used | `undefined` | Follows the default sink's mute flag (§11) | `validation`, `default-switch`, `pipewire-restart` |
| `Recorder.stopSpeakerMuteMonitor()` | Not used | `undefined` | Releases the callback | `validation` |
| `Recorder.isSpeakerMuted()` | Not used | `boolean` | Current mute state | `validation`, `default-switch` |
| `Recorder.computeAecLightMetrics(...)` | Not used | 49-key object | Port of the official algorithm (§12) | `aec-metrics` (117 tests) |
| `Recorder.checkScreenCapturePermission([cb])` | Not used | `0`/`1` | Backend probe (§13) | `screen` |
| `Recorder.checkScreenCapturePermissionFresh(...)` | `createOrFocusWindow-DNFqWFOl.js:1083` (macOS only) | `0`/`1` | Same | `screen` |
| `Recorder.requestScreenCapturePermission([cb])` | `createOrFocusWindow-DNFqWFOl.js:1088` (macOS only) | `boolean` | Same | `screen` |
| `Recorder.captureScreenImage([opts], [cb])` | `createOrFocusWindow-DNFqWFOl.js:1069` | `Buffer` or `Buffer[]` (PNG) | grim, XDG portal or XGetImage (§13) | `screen` |
| `Recorder.getWindowAtCursor([cb])` | Not used | `null` or window object | Compositor IPC or EWMH | `screen` |
| `Recorder.extractTextFromImage(png, [cb])` | Not used | throws | Validates arguments, then fails like the Windows build | `screen` |

`test/api-surface.mjs` scans every `Recorder.<name>(` and
`this.recorder.<name>(` call in the official JS and asserts that each one
exists on the Linux addon.

## 3. Events delivered to the `start` callback

The JS dispatches on `event.type`: `log`, `audio`, `encoded` and `fatal`
(`recordingService-Bn35UmTZ.js:487-503`). The key names below are all
present in the mac binary (mac strings 11080-11123, 11303-11314, 11859).

- **[D] Delivery.**
  - Events arrive on the JS thread in the order they were produced.
  - If the handler throws, the exception is caught and the next event is
    still delivered. The official addon also swallows it, but first prints
    `Error in JS <kind> callback: %s` to stderr, with kind `log`, `audio`,
    `aec`, `fatal` or `encoded` [O mac strings 11294, 11299, 11309-11315].
    Linux prints nothing, because addon output may only go to JS
    callbacks.
  - If the JS thread stalls, at most 3,000 `audio` events (30 s) are
    queued; beyond that, `audio` events are dropped. `log`, `encoded` and
    `fatal` events are never dropped.

### 3.1 `log`

`{type: 'log', level, message}`. `level` is `'debug'`, `'info'`,
`'warning'` or `'error'`, the levels of the official audio code [O mac
strings 11339, 11341, 11490; the screen statics use `warn` instead, §13.2].
Linux uses `debug` only for stream state transitions. The JS forwards both
fields to its logger, with `info` as the default level
(`recordingService-Bn35UmTZ.js:489-491`). The Linux messages
keep the wording of the official ones wherever an official string exists,
for example `Recorder started successfully.` (mac strings 11409),
`Stopping recorder...` (11410), `Recorder fully stopped and resources
released.` (11417), `Using user-selected microphone device: …` (11533),
`WorkerLoop: microphone stalled (no new data for …` (11471) and
`WorkerLoop: microphone recovered; data flowing again.` (11474).

### 3.2 `audio`

`{type: 'audio', data, container: 'pcm', size: 480, systemVolume,
microphoneVolume}`, one event per 10 ms chunk.

- `data` is a Buffer of 480 float32 samples: the mixed output (48 kHz,
  mono) that also goes to the encoder [D].
- `systemVolume` and `microphoneVolume` are the RMS (linear, 0-1) of the
  chunk of each source before processing; a source not in the chunk
  reports 0 [D].
- The JS uses only the two volumes, to drive the level meter
  (`recordingService-Bn35UmTZ.js:566-570`).

### 3.3 `encoded`

`{type: 'encoded', data, container: 'ogg', size, duration}`, one event per
Ogg page (§8).

- `data` is a Buffer holding one complete Ogg page; `size` is its length.
- `duration` is the audio the page adds, in milliseconds: 360 for an
  audio page, 0 for header and tag pages [D].
- The JS appends `data` to the recording file and sums `duration`. When
  the sum reaches the slice threshold (24 h − 5 s by default), it stops the
  recording and starts a new file (`recordingService-Bn35UmTZ.js:552-590`).
- The events are emitted only when `encoding: true` and the encoder
  started. Otherwise `start()` reports `container: 'pcm'` and only `audio`
  events flow.

### 3.4 `apm`

Only with `emitApmSignals: true`, which Plaud never sets [O grep]:
`{type: 'apm', lpb, mic, enh, lpbSampleRate, micSampleRate, enhSampleRate,
lpbChannels, micChannels, enhChannels}`. The keys are those of the mac
binary (mac strings 11303-11308), in that order. Per 10 ms chunk:

- `lpb` is the loopback (system audio, the far-end reference);
- `mic` is the raw microphone;
- `enh` is the processed microphone.

All three are float32 Buffers, 48 kHz, mono. The log line `emitApmSignals
enabled: JS will receive per-chunk APM (lpb/mic/enh) taps.` is the
official one (mac strings 11335).

### 3.5 `fatal`

`{type: 'fatal', code, message}`.

- **[O] What the JS does.** For `code === 'device_invalidated'` the JS
  calls `restart()` synchronously inside the callback. On `true` it keeps
  recording; otherwise, or when its restart circuit breaker trips, it stops
  the recording (`recordingService-Bn35UmTZ.js:507-532`).
- **[O] The code exists in the Windows binary:** `device_invalidated`
  (win strings 24753), together with `env_device_invalidated` (24701) and
  `recorder_restart_exhausted` (24621).
- **[D] When Linux sends it.** Linux sends exactly one
  `device_invalidated` per recording session when:
  - the PipeWire daemon goes away
    (`[sys:env_device_invalidated] PipeWire connection lost: …`);
  - a capture stream enters the error state
    (`[mic|sys:env_device_invalidated] … capture stream failed: …`);
  - `begin()` cannot add a stream.
- **[D] Unplugging a selected device is not fatal on Linux.** WirePlumber
  moves the stream to the default device (§6.3).

## 4. `start(options, callback)`

### 4.1 Options

What Plaud passes (`recordingService-Bn35UmTZ.js:248-256`):

```js
{ enableMicrophone: true, enableSystemAudio: true, microphoneVolume: 1,
  systemVolume: 1, audioDevices, encoding: true,
  audioProcessing: { backend: "webrtc" } }
```

Plaud always enables both sources. A source the user switched off is
paused right after `start` returns (`:296-301`).

| Option | Default | Linux behaviour |
|---|---|---|
| `enableMicrophone`, `enableSystemAudio` | `false` | Which capture streams exist. At least one must be `true`. |
| `microphoneVolume`, `systemVolume` | `1` | Gains applied in the mix (§7.5) |
| `encoding` | `false` | Ogg Opus encoding; without it, `container: 'pcm'` |
| `encodingBitRate` | `32000` | Opus CBR bitrate. 32 kbps gives the official 80-byte packets [O mac file] |
| `audioDevices.microphone`, `audioDevices.speaker` | system default | Device selection (§6) |
| `audioDeviceId`, `speakerDeviceId`, `audioDeviceUid`, `speakerDeviceUid`, `microphoneDeviceId`, `microphoneDeviceUid` | - | Legacy flat keys. They apply only to a side that `audioDevices` leaves unset [D]. |
| `audioProcessing.backend` or `audioProcessingBackend` | `webrtc` | `webrtc`/`apm`, `none`/`off`/`disabled`, or `plaud3a`, case-insensitive (§4.2) |
| `audioProcessing.plaudAecModelPath`, `plaudAnsModelPath` | env `PLAUD_AEC_MODEL_PATH`, `PLAUD_ANS_MODEL_PATH` | Only checked for presence (§4.2) |
| `emitApmSignals` | `false` | `apm` events (§3.4) |
| `debugDumpRawAudio` (also inside `audioProcessing`) | `false` | Raw dumps (§7.7) |

### 4.2 Validation and errors

The messages, error classes and check order are those of the official addon
[O oracle; `test/validation.test.mjs` "start() argument validation" holds
all 39 cases]:

- Already recording → `Error: Recorder is already running. Please stop it
  first.` (mac strings 11083).
- Fewer than two arguments, `options` not an object, or `callback` not a
  function → `TypeError: Start requires (options, callback).` (11085).
- An unknown backend → `TypeError: Unknown audioProcessing.backend: <x>`
  (11268). An empty string and `apple`/`vpio` are unknown too.
- `plaud3a` without both model paths → the official TypeError `Plaud 3A
  selected but model paths are missing. …` (11101). With both paths →
  `Error: Plaud 3A backend is not compiled in. Rebuild with
  PC_RECORDER_ENABLE_PLAUD_3A=ON.` (11102). The official macOS build does
  the same [O oracle].
- Invalid device values → the official TypeErrors, for example
  `audioDevices.microphone must be a non-negative integer within uint32
  range.`, `… must be an integer.`, `… must be a number, string, object,
  null, or undefined.`, `… object must include deviceId (number) or
  deviceUid (string).` (11275-11278).
- Neither source enabled → `TypeError: At least one of enableMicrophone or
  enableSystemAudio must be true.` (11114).
- **Start failure** → `Error: Recorder.start failed: Failed to start
  recorder. [<tag>] <detail>`. The prefix is official (11117, 11119). `tag`
  is `mic:init_failed` or `sys:init_failed`, depending on whether the
  microphone is enabled. Example: PipeWire unreachable.
  - **[O]** The JS extracts `[\w+:\w+]` from the message. For
    `mic:init_failed` and `sys:init_failed` it retries `start` up to 3
    times, 3 s apart (`recordingService-Bn35UmTZ.js:692-709`, `:813-815`).
  - **[D]** A start failure emits no `fatal` event, and no event reaches
    the callback after `start()` throws.

**[D] Deviation.** The official addon aborts the process (a `napi_throw`
with an exception already pending) for negative or non-integer device ids
in `audioDevices`, and for `checkSystemAudioPermission()` without a
callback [O oracle: the process died]. Linux throws the matching TypeError,
or skips the logs.

### 4.3 Start sequence [D]

1. Connect to PipeWire (2 s timeout), bind the registry, round-trip twice.
2. Create the APM (unless the backend is `none`) and the encoder. An
   encoder failure falls back to PCM with the official warning `Warning:
   Failed to create encoder, falling back to PCM` (mac strings 11115).
3. Create one capture stream per enabled source, both in one `node.group`
   (§7.1), resolving the selected devices (§6.3).
4. Log `Recorder started successfully.` and return.

A missing microphone or output is not a start failure: `No microphone is
available yet; recording continues and picks one up when it appears.` The
stream links as soon as WirePlumber finds a device.

## 5. Recording control

- **`RecorderPauseMode`**: `Microphone = 0`, `SystemAudio = 1`, `Mixed = 2`
  (`recordingService-Bn35UmTZ.js:150-155`). `pause()` and `begin()` take
  an optional mode. The low byte of the value converted to uint32 selects
  0 = microphone, 1 = system audio, anything else both; no argument means
  both [O mac disassembly; oracle: `pause(5)`, `pause(-1)` and `pause(1.5)`
  are accepted]. A non-number throws `TypeError: pause expects an optional
  numeric mode.` (mac strings 11125), and the same for `begin`. On an idle
  recorder both return `undefined` [O oracle].
- **`pause(mode)`** is flag-only. The stream stays linked, its samples are
  dropped, and the log reads `StopMicrophone(): microphone capture paused
  (flag-only).` (mac strings 11419). With one source paused, chunks contain
  only the other one. With both paused, nothing is produced. The JS fills
  that gap later with `fillSilence` (`recordingService-Bn35UmTZ.js:597-620`).
- **`begin(mode)`** clears the side's backlog and resumes it. The backlog
  is what the source captured before the call; the worker drops it (§7.2).
  - For a source that was not enabled at `start`, Linux adds its capture
    stream to the running pipeline. macOS restarts its recorder instead
    (`Restarting recorder …`, mac strings 11448-11449) [D: a PipeWire
    stream can be added without touching the other one].
  - If the stream cannot be added, the log reads `BeginMicrophone failed:
    recorder restart failed. …` (mac strings 11429) and a `fatal
    device_invalidated` follows.
- **`fillSilence(ms)`**:
  - a non-number throws `TypeError: fillSilence requires a durationMs
    number argument.` (11131);
  - any number is accepted; an idle recorder ignores it [O oracle: `-5`,
    `0`, `1.5`, `NaN`]. While recording, values below 10 and NaN do
    nothing [D];
  - otherwise `floor(ms/10)` chunks of silence go straight to the encoder,
    capped at 24 h [D: the JS computes the gap from wall-clock time];
  - it has no effect without the encoder.
- **`setEncodingPaused(on)`**:
  - requires a boolean, else `TypeError: setEncodingPaused requires a
    boolean argument.` (11130);
  - while paused, capture, APM and `audio` events continue, but the encoder
    receives nothing;
  - Plaud uses it for the user's pause button (`:383-397`);
  - the flag survives until `stop()`; a value set before `start()` applies
    to the next recording [D].
- **`setApmEnabled(on)`**:
  - requires exactly one boolean, else `TypeError: setApmEnabled requires a
    single boolean argument.` (11129);
  - `false` bypasses the APM, so the raw microphone is mixed;
  - it resets to enabled on every `start` [D].
- **`stop()`**:
  - returns `undefined` on an idle recorder [O oracle] and `true` after
    stopping a recording [D];
  - it stops capture, lets the worker process every buffered sample,
    finishes the Ogg stream (§8), and delivers all remaining events,
    including the final pages, before it returns;
  - pending `fatal` events are discarded;
  - **[D]** The JS calls `ws.end()` right after `stop()` returns
    (`recordingService-Bn35UmTZ.js:366-378`), so pages delivered later
    would be written after the end of the stream.

## 6. Devices

### 6.1 Identity

- **[D] uid = PipeWire `node.name`**, for example
  `alsa_input.usb-Vendor_USB_Microphone_0000-00.mono-fallback`. It is
  stable across reboots and re-plugs, like the CoreAudio UID it replaces.
- **[D] id = PipeWire global id.** It is a number, as on macOS, but not
  stable across a PipeWire restart.
- **[D] name = `node.description`**, falling back to `node.nick`, then to
  `node.name`.
- **[D]** The AudioMonitor addon (smart microphone and speaker) uses the
  same uid, so its `micEvent` and `speakerEvent` uids pass to
  `setAudioDevice` unchanged (agreed with that slice).

### 6.2 Lists

- **[O] Shapes.** `list*DevicesDetailed()` returns objects with exactly
  the keys `id` (number), `name`, `uid` (strings) and `isDefault`
  (boolean), in that order. `list*Devices()` returns the names in the same
  order. Exactly one entry is the default when the list is not empty
  [O oracle; `validation` "device lists have the official shapes"].
- **[D] Contents.** The microphones are the nodes of media class
  `Audio/Source`, `Audio/Source/Virtual` or `Audio/Duplex`; the speakers
  are `Audio/Sink` or `Audio/Duplex`. Monitor sources and stream nodes are
  excluded. `isDefault` follows WirePlumber's `default.audio.source` and
  `default.audio.sink` metadata.
- **[D] No PipeWire.** The lists are empty; nothing throws.

### 6.3 Selection, pinning and fallback

- **[O] Plaud always passes uids.** The microphone is either the user's
  choice or the "smart" microphone (`smartSelectedMicAtom`). When the
  selected device disappears, the JS falls back to the smart choice
  (`recordingService-Bn35UmTZ.js:110-123`,
  `lifecycleService-GDc1zMWu.js:620-660`).
- **[D] Resolution.**
  - A string is looked up as a `node.name`; an all-digit string that
    matches no name is tried as a global id.
  - A number is a global id. An object is read by precedence: `deviceId`
    (number) > `deviceUid` (string) > `uid` (string) > `id` (number) [O
    oracle: `{deviceId: 5, deviceUid: 'x'}` selects id 5].
  - A sink uid never selects a microphone: the sides are checked.
- **[D] Pinned selection.** A selected device gets `target.object = <uid>`,
  `node.target = <id>` (for older session managers) and
  `node.dont-move = true`.
  - Without `node.dont-move`, WirePlumber 0.5 turns a stream that is linked
    to the current default into a default-follower (PulseAudio semantics,
    its `checkFollowDefault` hook). A user who picked the device that is
    currently the default would then be moved when the default changes.
  - The official recorder stays on its selected device [O: the mac
    `SetPreferredDevices` path only restarts on an explicit selection,
    mac strings 11444-11449].
- **[D] Unplug and re-plug.** `target.object` drives both:
  - when the pinned device disappears, WirePlumber links the stream to the
    current default with no gap in the recording;
  - when the device comes back, it moves the stream back.
  - `test/hotplug.test.mjs` "selected devices unplugged and re-plugged
    mid-recording" asserts both moves and an uninterrupted Ogg file.
- **[D] A uid that names no current node** is still pinned (`target.object`
  only, not all-digit):
  - the stream records from the default meanwhile;
  - the log reads `Microphone device "<uid>" is not available. Falling back
    to system default.` (official wording, mac strings 11455);
  - the stream moves to the device once it appears, for example after a
    PipeWire restart.
- **[D] No selection** means no target, and the stream follows the
  default: WirePlumber re-links it when the user changes the default
  (`test/default-switch.test.mjs`, sink and source cases).

### 6.4 `setAudioDevice(selection)`

Plaud calls it with `{microphone?: uid, speaker?: uid}`
(`recordingService-Bn35UmTZ.js:421-433`). The parsing and return values are
the official ones [O oracle; `validation` "setAudioDevice() parsing and
results", 40 cases]:

- `undefined`, `null` or no argument → both sides back to the default;
  returns `{microphone: null, speaker: null}`.
- A number → that microphone id; returns the number. Out of range → `deviceId
  must be within uint32 range.`; a fraction → `deviceId must be an integer.`
- A string → that microphone uid; returns the resolved id when the uid
  names a current microphone, else the string.
- An object with `microphone` and/or `speaker` → returns
  `{microphone, speaker}`, each the resolved id, the string or `null`.
- Another object is one microphone device, read as in §6.3; it returns
  `{microphone, speaker: null}`. An object without a usable key, and an
  array, throw `device object must include deviceId (number) or deviceUid
  (string).`; a bad value throws, for example, `device.id must be within
  uint32 range.` or `device.deviceId must be an integer.`
- Anything else → `setAudioDevice expects number, string, object, or
  null.` (mac strings 11128).

**[D] While recording**, only a side whose pin target changes gets a new
stream. The log reads `Switching microphone to <uid>.`. Re-sending the same
uids, which Plaud does on every smart-device change, is a no-op. An idle
recorder only returns the echo.

### 6.5 `watchMicrophoneDevices(cb)`

- **[O] Registration.** It returns `true` and may be called again;
  `stopWatchingMicrophoneDevices()` also returns `true`, even twice. A call
  without a function throws `TypeError: watchMicrophoneDevices requires a
  callback` (mac strings 11833) [O oracle].
- **[O] How Plaud uses it.** The JS logs the names it receives and
  re-reads the detailed list (`recordingService-Bn35UmTZ.js:129-132`).
- **[D] Linux.**
  - `cb(names)` gets the current microphone names after any
    `Audio/Source` node is added, removed or renamed, or the default
    source changes.
  - The calls are debounced by 150 ms, so a device that brings several
    nodes produces one call.
  - The watcher reconnects after a PipeWire restart and reports the new
    list (§10).

## 7. Audio pipeline

### 7.1 Streams [D]

- Each source is a `pw_stream` capture stream: F32, 48 kHz, mono,
  connected with `PW_STREAM_FLAG_AUTOCONNECT` so that WirePlumber links
  it, and `PW_STREAM_FLAG_RT_PROCESS`. PipeWire converts and downmixes as
  needed.
- System audio is captured with `stream.capture.sink = true` from the
  monitor of an output (the selected sink, or the default sink).
- Both streams carry one `node.group`. PipeWire then drives them from the
  same graph cycle, so their sample clocks match and the microphone and
  the loopback stay aligned for echo cancellation.
- Node names: `plaud-recorder-microphone` and
  `plaud-recorder-system-audio`. `application.name` is `Plaud`.

### 7.2 Worker [D]

- The worker processes 10 ms chunks (480 samples):
  - with both sources running, it waits until both have a chunk;
  - a source that delivers nothing for 60 ms counts as stalled, and the
    other is processed alone. The threshold is stretched to three graph
    quanta for large quanta. The stall and recovery log lines are the
    official ones;
  - if one source's backlog exceeds the other's by more than 100 ms (for
    example after a relink), the excess is dropped and a `… ahead of … by N
    ms; realigned.` warning is logged.
- Ring buffer overflow (a JS-independent stall over 2.7 s) drops samples
  and logs a warning.
- **Single consumer.** Each ring has one producer, the stream's `process`
  callback, and one consumer, the worker. Only the worker moves a ring's
  read index; the producer takes no lock. `begin()` and `restart()` run on
  the JS thread, so they do not drop samples themselves:
  - they record the ring's write index and publish it as a drop request;
  - the worker applies the request before it reads the side again and
    drops only the samples written before that index;
  - `begin()` publishes the request before it re-enables the side.
- **Misuse check.** The ring records its first consuming thread. A consumer
  call from another thread is counted, and the worker logs `Internal
  error: the <side> ring buffer was consumed from more than one thread (N
  times).` `concurrency.test.mjs` hammers `pause`/`begin`/`restart` while
  audio flows and asserts that this log never appears. Run against the
  earlier code, which dropped samples on the JS thread, it failed [O].

### 7.3 Echo cancellation decision

- **[O] Official rule.** AEC runs only when both sources are mixed and the
  default output is the built-in speaker: `Default output transport is
  BuiltIn (enabling AEC if mixing)`, else `Non-BuiltIn (disabling AEC)`
  (mac strings 11384-11386). Headphones, Bluetooth and external outputs
  cannot leak into the built-in microphone in the way a laptop speaker
  does.
- **[D] Linux rule.** The question is whether the captured output can be
  heard by the microphone. AEC is disabled when the captured sink is:
  - Bluetooth (`bluez_` node name, `device.bus = bluetooth` or API
    `bluez5`);
  - a headphone-type form factor (`headphone`, `headset`, `handset`,
    `hands-free`) on the node or its device;
  - an ALSA device whose active output route is a headphone or headset
    port.
  
  Anything else counts as loudspeaker-type, including USB and HDMI
  speakers and virtual sinks, and gets AEC. AEC is also on when nothing is
  known about the output.
- **[D]** The decision is made on start and re-made whenever the captured
  sink, its route or its properties change, and on default switches.
  - The log keeps the official prefix: `Default output transport is
    loudspeaker-type output (enabling AEC if mixing)` or `… headphone-type
    output (form factor headphone) (disabling AEC)`.
  - `Echo cancellation enabled.` / `disabled.` is logged when the APM
    changes.
- **[O] Measured.** `test/aec.test.mjs` plays a speech-like signal (noise
  with a 4 Hz syllable envelope) into a virtual loudspeaker and feeds the
  sink back into a virtual microphone with a delay. Over the last 3 s of a
  6 s recording, the processed microphone (`enh`) carries 27.2 dB less
  energy than the raw microphone (`mic`) with AEC. With the same path on a
  sink tagged `device.form-factor=headphone` (AEC off), the difference is
  5.4-5.5 dB across runs, from noise suppression and gain control alone.
  The test requires ≤ −10 dB with AEC and a gap of at least 10 dB between
  the two cases.

### 7.4 WebRTC Audio Processing configuration

- **[O] The official settings** come from the mac disassembly of
  `MacRecorder::InitializeOrUpdateApmLocked` (0x25598-0x25718):
  - `maximum_internal_processing_rate` 32000 (`#0x7d00` @0x256a8);
  - AGC2 adaptive digital: headroom 5 dB, max gain 30 dB, initial gain
    8 dB, max gain change 3 dB/s (float literals at 0xaa160);
  - max output noise level −50 dBFS (0xa9fd0);
  - 48 kHz mono stream configs (`#0xbb80`).
- **[D] Remaining flags.** The boolean fields of the older WebRTC
  `Config` layout were mapped onto 2.1:
  - high-pass filter on;
  - AEC3 (`echo_canceller`, not mobile mode) toggled by §7.3;
  - noise suppression on, level low;
  - AGC1, the input volume controller, the pre-amplifier, capture level
    adjustment and the transient suppressor off.
- **[D] Stream delay.** The reverse stream is the system-audio chunk, and
  the stream delay is set to 15 ms. Because of `node.group` (§7.1), the
  remaining delay is the acoustic path, which AEC3's delay estimator
  covers.
- **[D] Vendoring.**
  - Version 2.1 is the one both official builds use (§1).
  - Arch packages 2.1 (`extra/webrtc-audio-processing 2.1-9`) next to the
    legacy 0.3 and 1.x packages, but which version other distributions
    ship varies [?]. Vendoring gives the same processing on every
    distribution and no runtime dependency.
  - `scripts/build-deps.mjs` downloads webrtc-audio-processing 2.1 and its
    abseil-cpp 20240722.0 subproject, each pinned by sha256 in the script.
    It builds them with meson as static PIC libraries with hidden
    visibility, and writes the library list and flags for CMake to
    `deps/webrtc-apm.json`. The addon links them with `--exclude-libs,ALL`,
    so they add no runtime dependency and export no symbols.

### 7.5 Mixing

- **[O] Two sources.** The output is the system audio and the processed
  microphone, mixed with adaptive gains. The algorithm comes from the mac
  disassembly of `Util::ComputeAdaptiveMix` (@0x18d40) and
  `Util::mixChannelsWeighted` (@0x177c4); the Linux code is
  `src/audio/adaptive_mix.h`:
  - per source, an envelope follower on the chunk RMS (attack 0.7, release
    0.9);
  - target gain `0.1/(env+1e-6)`, clamped to [0.2, 2.5];
  - the microphone is ducked by 0.5 while the system envelope exceeds
    0.02;
  - gains are smoothed (`0.3·target + 0.7·previous`) and multiplied by the
    user volumes;
  - `out = tanh(0.9·(sys·gS + mic·gM)/(gS + gM))`.
- **[D] One source.** The output is that source times its volume, clipped
  to [−1, 1].

### 7.6 Muted output

- **[O]** When the captured output is muted or at volume 0, the official
  recorder zero-fills the system audio before processing: `WorkerLoop:
  hardware output muted/volume=0, zero-filling system audio buffer before
  APM processing` (mac strings 11563; Windows has the same check, win
  strings 24785-24793).
- **[D]** Linux does the same, based on the sink's `mute` flag and channel
  volumes from its `Props`. In PipeWire, the monitor of a muted sink can
  still carry the unmuted signal, depending on `monitor.channel-volumes`, so
  without the zero-fill the recording would contain audio the user muted.
  `test/default-switch.test.mjs` "muting the captured output records
  silence until unmuted" asserts this.

### 7.7 Debug dumps [D, names as on macOS]

- `debugDumpRawAudio: true` writes `sys`, `mic` and `mix` dumps to
  `$TMPDIR` (default `/tmp`) as `linux_recorder_<kind>_<stamp>.raw`, Float32
  PCM, 48 kHz, mono. macOS writes `/tmp/mac_recorder_…` (mac strings 11535,
  11362).
- `PLAUD_PRE_APM_DUMP=1` adds the `mic_raw`, `mic_post3a`, `sys_raw` and
  `out_mix` dumps (mac strings 11334, 11367).
- Neither is used by Plaud.

## 8. Ogg Opus container

- **[O] The layout of a file written by the official app** (mac file; the
  official encoder classes are `MacOpusEncoder`, mac strings 11852, and
  `WinOpusEncoder`). Linux reproduces it page for page:
  1. Page 1: `OpusHead` on serial S (BOS), 19 bytes: version 1, 1 channel,
     pre-skip 0, input rate 16000, gain 0, mapping family 0.
  2. Page 2: a BOS page on serial S+1 holding one 300-byte packet:
     `PLAUD.AI` followed by zeros (segments 255 + 45).
  3. Page 3: `OpusTags` on serial S. The vendor is `libopus 1.1.4-fixed`
     (mac strings 11856), with one 68-byte, NUL-padded comment that starts
     with `VENDOR=PALUD.AI` (sic, mac strings 11857), plus a trailing NUL
     byte.
  4. Then audio pages alternate with tag pages:
     - an audio page holds 18 packets of 20 ms, i.e. 360 ms. The packets
       are hard CBR, 80 bytes at 32 kbps, wideband, with TOC config 9
       (SILK) or 23 (CELT). The granule advances by 17280 per page;
     - each audio page is followed by a page on serial S+1 holding one
       23-byte zero packet, with the same granule position.
- **[O]** The JS concatenates the pages into the file and needs nothing
  else (`recordingService-Bn35UmTZ.js:552-563`).
- **[D] End of stream (deviation).** The official file has no EOS page.
  Linux sets `e_o_s` on the last audio page and the last tag page:
  - on `stop()`, the partial frame is padded with silence, and the last
    page is filled with silence up to 18 packets;
  - if the recording stops exactly on a page boundary, one extra page of
    silence (360 ms) carries the flag;
  - no zero-length packets are written, because RFC 6716 requires every
    Opus packet to be at least 1 byte.
  
  With the flag, players and `ffprobe` see a well-formed, finished stream
  (`test/record.test.mjs` "Ogg container layout matches the official
  encoder"). Readers that ignore EOS are unaffected.
- **[D] Serial numbers** are random per recording; S+1 is the tag stream.

## 9. System audio permission

- **[O] How Plaud uses it.** Only macOS calls these statics. On other
  platforms the JS assumes Granted (`lifecycleService-GDc1zMWu.js:236-238`).
- **[O] Official logs** (oracle; mac strings 11687-11703):
  - `checkSystemAudioPermission(cb)` returns `0` (Granted) or `1` (Denied).
    It logs `Starting system audio permission check`, then `System audio
    permission status: Granted|Denied`;
  - `requestSystemAudioPermission(cb)` returns a boolean. It logs `Starting
    system audio permission request`, then `System audio permission request
    result: granted|denied`;
  - the callback receives `{level, message}`.
- **[D] Linux.**
  - Linux has no per-application audio capture permission outside
    sandboxes, so the status is "PipeWire is reachable". The check makes a
    real connection and a registry round trip;
  - when PipeWire is unreachable, the log reads `PipeWire is not reachable:
    <why>` (level `error`) before `… Denied` / `… denied`;
  - without a callback the logs are skipped. The official addon aborts the
    process in that case [O oracle].

## 10. PipeWire loss and `restart()`

- **[D] Detection.** The engine watches the core `error` event and the
  stream states. When the daemon dies (crash, `systemctl --user restart
  pipewire`, package update), the recorder:
  - logs `PipeWire connection lost: …`;
  - sends one `fatal` `{code: 'device_invalidated', message:
    '[sys:env_device_invalidated] PipeWire connection lost: …'}`;
  - stops producing chunks. The already encoded audio is kept.
- **`restart()`** returns `false` on an idle recorder [O oracle]. While
  recording, it:
  1. tears down the streams and the connection;
  2. reconnects, retrying until 3 s have passed. Each attempt is logged as
     `LinuxRecorder::Restart - attempt N …` (official prefix
     `MacRecorder::Restart`, mac strings 11438);
  3. recreates the streams with the same selection, APM and encoder state;
  4. logs `LinuxRecorder::Restart - recorder restarted successfully.` and
     returns `true`.
  
  The encoder is kept, so the Ogg stream continues across the restart in
  one file. If the budget runs out, it logs `LinuxRecorder::Restart -
  restart failed: …`, stops the recorder (final pages with EOS are
  delivered) and returns `false`. The JS then stops the recording.
- **[D] Calling it inside the fatal callback is safe**, which is what the
  JS does. The event queue lock is not held while a callback runs, so a
  restart can neither deadlock against the PipeWire thread nor lose
  events. The call blocks the JS thread for at most 3 s.
- **[D] Device ids** change across a daemon restart. Pinned uids survive it
  (§6.3): the streams record from the default until the selected devices
  reappear.
- **[D] Tests.** `test/pipewire-restart.test.mjs` kills a private daemon:
  - while recording: one fatal; `restart()` from outside and from inside
    the callback; audio flows again and the Ogg file stays valid;
  - with no daemon coming back: `restart()` returns `false` after about
    3 s;
  - before start: `start()` throws `[mic:init_failed]`/`[sys:init_failed]`,
    the permission is Denied and the lists are empty;
  - with the watcher and mute monitor running: both reconnect.

## 11. Speaker mute monitor

None of these statics is used by Plaud [O grep]. Behaviour matches the
oracle (`validation` "speaker mute monitor lifecycle"):

- **[O] Lifecycle.** `startSpeakerMuteMonitor(cb)` returns `undefined`.
  - Without a function → `TypeError: startSpeakerMuteMonitor requires a
    callback function.`
  - A second start → `Error: Speaker mute monitor already started.` (mac
    strings 11827).
  - `isSpeakerMuted()` returns a boolean, or throws `Error: Speaker mute
    monitor is not started.` (11829).
  - `stopSpeakerMuteMonitor()` returns `undefined`, also when not started.
- **[O] Events.** `{type: 'log', obj: {level, message}}` and `{type:
  'status', obj: {muted}}` (mac strings 11813, 11830-11831). On start:
  `Added mute listener to device <id>` (11820), then `LinuxSpeakerMuteMonitor
  started.` (official: `MacSpeakerMuteMonitor started.`, 11807), then a
  `status` event.
- **[D] Linux.**
  - The monitored value is the default sink's `mute` flag from its `Props`;
    the volume is ignored, as in the official monitor.
  - On a change, the log reads `Mute state changed on device <id>:
    muted|unmuted`, followed by a `status` event.
  - When the default output changes, the log reads `Removed mute listener
    from device <old>`, `Default output device changed.`, `Added mute
    listener to device <new>`, and the state is re-evaluated.
  - A sink without `Props` logs `Device <id> does not expose mute
    property.` and counts as unmuted.

---

## 12. `Recorder.computeAecLightMetrics(lpbBuf, lpbSr, micBuf, micSr, enhBuf, enhSr, [options])`

Offline "light" AEC quality metrics for three mono float streams:
loopback (the far end as played, `lpb`), raw microphone (`mic`) and the
enhanced, post-APM output (`enh`). It is synchronous, has no side effects
and takes no log callback. Plaud's own JS never calls it:
`computeAecLightMetrics` does not appear anywhere under
`out-global-online/` [O grep, 0 matches]. The Linux addon exposes it with
identical behaviour for parity.

Evidence in this section: arm64 disassembly addresses in
`recorder_mac.node`, mac strings lines, and oracle calls captured in
`test/fixtures/aec-metrics/expected.json`.

### 12.1 Arguments and errors

- Fewer than 6 arguments → `TypeError: computeAecLightMetrics requires
  (lpbBuf, lpbSr, micBuf, micSr, enhBuf, enhSr, [options]).` [O 0x8af4,
  mac strings 11171, oracle].
- Validation order: lpbBuf, lpbSr, micBuf, micSr, enhBuf, enhSr [O
  0x8b48-0x8dac, oracle probes `order*`].
- **Buffers.**
  - Any value that N-API reports as a buffer is read as float32 samples:
    every ArrayBufferView in current Node, i.e. Buffer and all TypedArrays.
  - Sample count = `elementLength × elementSize >> 2`. Element sizes come
    from a table indexed by `napi_typedarray_type`; unknown kinds count
    as 0 [O lambda `$_0` @0xe690, table @0xa9f58; oracle: Float64Array,
    Int16Array and BigInt64Array are accepted, Float16Array → 0 samples].
  - A DataView passes the buffer check but has no typed-array info, so it
    throws N-API's `Error: Invalid argument`, unwrapped [O oracle].
  - Anything else → `TypeError: <lpbBuf|micBuf|enhBuf> must be a
    Buffer<float> or Float32Array.` [O 0xe7e0, oracle]. ArrayBuffer and
    Array are rejected [O oracle].
  - Linux copies unaligned Buffer slices before reading them [D].
- **Rates.** They must be `typeof number`, otherwise `TypeError: lpbSr must
  be a number` (likewise `micSr`, `enhSr`) [O 0x8bb0/0x8ccc/0x8fdc,
  oracle]. Values are converted with ToInt32: NaN and ±Infinity → 0,
  fractions are truncated, values wrap modulo 2^32 [O `Int32Value`
  @0x8dcc-0x8e0c, oracle].
- **Options.**
  - `options` is read only when a 7th argument exists and is an object or
    function [O `IsObject` @0x8e98, oracle `sevenFunction`].
  - Double options use `Has`, then one `Get`. Integer options use `Has`,
    then `Get` for the type check, then another `Get` for the value.
    Non-number values are ignored, and inherited properties count [O
    0x8eb8-0x92d0, oracle Proxy trace].
  - Read order: frameMs, hopMs, maxDelayMs, corrThresh, lpbRelThreshDb,
    delayEstimationMaxSec, residualEchoCorrThresh, noiseMaxDbAboveFloor,
    speechMinDbAboveNoise, ansSuppressionDbThreshold1,
    ansSuppressionDbThreshold2, agcSpeechTargetDbfs, agcSpeechToleranceDb,
    downsampleFactorForDelay, targetSampleRate, minNoiseFrames,
    minSpeechFrames [O].
  - Exceptions thrown by getters or Proxy traps propagate unchanged
    [O oracle].
- **Computation errors** are rethrown as `Error: computeAecLightMetrics
  failed: <what()>` [O 0x9b98, mac strings 11195]. The only reachable case
  is `ResampleLinear: invalid sample rates`. It is raised when a stream must
  be resampled and its rate, or lpbSr used as the target, is ≤ 0 [O
  0x18f68, oracle `micSrZero`, `enhSrNeg`, `lpbSrZeroTsr`].

### 12.2 Option defaults [O constants @0xa9eb0-0xa9ef0, stores 0x8e14-0x8e6c]

| Option | Default |
|---|---|
| frameMs | 20 |
| hopMs | 10 |
| maxDelayMs | 500 |
| corrThresh | 0.6 |
| lpbRelThreshDb | −40 |
| downsampleFactorForDelay | 4 |
| delayEstimationMaxSec | 10 |
| targetSampleRate | 0 (use lpbSr) |
| residualEchoCorrThresh | 0.3 |
| noiseMaxDbAboveFloor | 3 |
| speechMinDbAboveNoise | 10 |
| minNoiseFrames | 20 |
| minSpeechFrames | 20 |
| ansSuppressionDbThreshold1 | 3 |
| ansSuppressionDbThreshold2 | 6 |
| agcSpeechTargetDbfs | −18 |
| agcSpeechToleranceDb | 6 |

### 12.3 Result

A plain object with 49 keys, always all present and in this order [O
0x95a0-0x9b68, oracle]:

- integers: `sr, delayMicSamples, delayEnhSamples, totalFrames,
  farendActiveFrames, echoDominantFrames, nearendOnlyFrames,
  ansNoiseFrames, agcSpeechFrames`;
- a number, or `null` when not computed (std::optional) [O lambda `$_2`
  @0xea24]: `farendActiveRatio, echoDominantRatio, nearendOnlyRatio,
  coherentEchoSuppressionDbFarendActive,
  coherentEchoSuppressionDbFarendActiveGainNormalized,
  mixtureEnergyReductionDbFarendActive, meanAbsCorrMicFarendActive,
  meanAbsCorrEnhFarendActive, corrSuppressionDbFarendActive,
  residualEchoRatioFarendActive, erleDbEchoDominant,
  coherentEchoSuppressionDbEchoDominant,
  coherentEchoSuppressionDbEchoDominantGainNormalized,
  meanAbsCorrMicEchoDominant, meanAbsCorrEnhEchoDominant,
  corrSuppressionDbEchoDominant, residualEchoRatioEchoDominant,
  ansNoiseRatio, agcSpeechRatio, ansSuppressedNoiseRatio3db,
  ansSuppressedNoiseRatio6db, ansSuppressedTotalRatio3db,
  ansSuppressedTotalRatio6db, agcSpeechLevelInRangeRatio,
  agcSpeechLevelInRangeTotalRatio, noiseFloorDbfsMic, noiseFloorDbfsEnh,
  noiseReductionDb, noiseEnergyReductionDb, speechLevelDbfsMicMean,
  speechLevelDbfsEnhMean, speechLevelDbfsMicStd, speechLevelDbfsEnhStd,
  speechLevelStdReductionDb, agcGainDbMean, agcGainDbStd,
  agcGainChangeDbPerSec, snrImprovementDb, enhClipFraction, enhPeakDbfs`.

### 12.4 Algorithm (`Util::ComputeAecLightMetrics` @0x197a8)

1. **Empty input.** If any buffer is null or empty → all integers 0, all
   optionals null [O 0x19928-0x19944, oracle `emptyAll`, `emptyEnh-none`].
   The working rate is `sr = targetSampleRate > 0 ? targetSampleRate :
   lpbSr`. If `sr < 1` → the default result [O 0x19950, oracle `lpbSrNeg`].
2. **Resampling.** Each stream is resampled to `sr` by linear interpolation
   (`Util::ResampleLinear` @0x18e1c):
   - output length = round-half-away((dst/src)·len);
   - a length of 1 yields a single 0 sample;
   - positions beyond the last sample repeat the last sample [O].
   
   All three streams are truncated to the shortest length n. If n = 0,
   only `sr` is set [O 0x19b94, oracle `resampleToZero-tsr8k`].
3. **Delays.** `delayMicSamples = EstimateDelaySamplesGccPhat(mic, lpb)`
   and `delayEnhSamples = EstimateDelaySamplesGccPhat(enh, lpb)` [O
   0x19bd4/0x19bfc]. The value is positive when the microphone hears the
   far end later [O oracle: `s16-none` = +37 for a 37-sample echo,
   `s16Lead` = −29]. GCC-PHAT (@0x18fac):
   - only the first `min(len, round(sr·delayEstimationMaxSec))` samples are
     used (no limit when ≤ 0 or NaN);
   - the downsampling factor is `max(1, downsampleFactorForDelay)`, forced
     to 1 when `sr/factor < 1001`; the signals are block-averaged;
   - a radix-2 FFT of size nextpow2(2·len) (@0x1bdd4; errors `FFT:
     real/imag length mismatch`, `FFT: length must be a power of two`). The
     cross spectrum X·conj(Y) is divided by max(|·|, 1e-12), then
     inverse-transformed;
   - coarse peak: |r[lag]| is maximized over ±min(round(maxDelayMs/1000 ·
     sr/factor), N/2), taken as uint32. A negative or saturated maximum lag
     gives a window of 0 [O oracle: `maxDelayNeg` and `maxDelayHuge` behave
     like `maxDelay0`];
   - refinement at the full rate over coarse·factor ± max(2, 2·factor),
     maximizing the |normalized correlation| of ref[i+lag] and sig[i]. In
     both stages the first maximum wins [O 0x193f0-0x1953c, oracle mutation
     check].
4. **Alignment.** lpb is shifted by each delay with zero fill, giving one
   copy aligned to the mic and one aligned to the enh [O 0x19c38-0x19d34].
5. **Framing.** F = round(frameMs·sr/1000) and H = round(hopMs·sr/1000);
   both must be ≥ 1 as int32, and n ≥ F. Otherwise everything after the
   delays is skipped, including the clip metrics (40 nulls) [O
   0x19d38-0x19d84, oracle `frame0`, `hopNeg`, `frameNaN`, `frameHuge`,
   `short-none`]. totalFrames = (n−F)/H + 1.
6. **Per frame** (double accumulation), for mic and for enh [O
   0x1a0c0-0x1a19c]:
   - lpb, mic and enh power = Σx²/F;
   - correlation ρ = Σ(m·l)/(√(Σm²·Σl²) + 1e-12);
   - coherent echo power = Σl²·(Σml/(Σl² + 1e-12))²/F.
7. **Classification.**
   - A frame is far-end active when lpbPow > max(1e-12, max(lpbPow) ·
     10^(lpbRelThreshDb/10)) [O 0x1a1c0].
   - It is also echo dominant when |ρ_mic| ≥ corrThresh.
   - Residual echo means |ρ_enh| ≥ clamp(residualEchoCorrThresh, 0, 1);
     NaN never matches [O 0x1a324-0x1a3d8, oracle `resNaN`].
   - Sums get +1e-12 before the dB ratios. nearendOnlyFrames = max(0,
     total − farend).
   - farendActiveRatio and nearendOnlyRatio divide by totalFrames and are
     set when frames exist.
   - echoDominantRatio and residualEchoRatio* divide by their respective
     frame counts and are null when that count is 0.
   - The coherent, mixture and ERLE values are 10·log10(Σmic/Σenh). The
     GainNormalized variants weight the enh echo power per frame by
     (micPow+ε)/(enhPow+ε).
   - corrSuppression = 20·log10((mean|ρ_mic|+ε)/(mean|ρ_enh|+ε)) [O
     0x1a428-0x1a634].
8. **Noise and speech** use only the frames without far-end activity (the
   candidates). Frame level = 10·log10(pow + 1e-12). If there are fewer
   than max(1, minNoiseFrames + minSpeechFrames) candidates, ansNoiseFrames
   = agcSpeechFrames = 0 [O 0x1a814-0x1a84c, oracle `hugeMins`]. Otherwise:
   - floor = the 10th percentile of the candidates' mic dB, with linear
     interpolation;
   - a noise frame has micDb ≤ floor + max(0, noiseMaxDbAboveFloor); a
     speech frame has micDb ≥ floor + max(0, speechMinDbAboveNoise); a
     frame can be both;
   - suppressed noise: micDb − enhDb ≥ max(0, threshold1 or threshold2).
     In range: |enhDb − agcSpeechTargetDbfs| ≤ max(0, tolerance) [O
     0x1aad4-0x1afe4];
   - the noise ratios need ≥ max(1, minNoiseFrames) noise frames. The
     floors are medians; noiseReductionDb = floorMic − floorEnh;
     noiseEnergyReductionDb = 10·log10(ΣmicPow/ΣenhPow);
   - the speech statistics need ≥ max(2, minSpeechFrames) frames: Welford
     mean and sample standard deviation; agcGainDb = enhDb − micDb;
   - agcGainChangeDbPerSec = the mean |Δ| of a gain smoothed with τ = 0.2 s
     over consecutive speech frames, with dt ≥ max(hopMs/1000, 1e-6);
   - snrImprovementDb = (enhMean − floorEnh) − (micMean − floorMic) [O
     0x1b09c-0x1b464];
   - ansNoiseRatio and agcSpeechRatio = count/totalFrames [O 0x1b500].
9. **Clipping**, on enh at the working rate: enhClipFraction = the share of
   samples with |x| ≥ 0.999; enhPeakDbfs = 20·log10(peak + 1e-12) [O
   0x1b560].

### 12.5 Linux implementation notes

- **[D] Code.** C++17 in `src/aec_metrics/`. The pure core
  (`aec_metrics.{h,cpp}`, no N-API) repeats the reference arithmetic
  operation by operation:
  - the same accumulation order;
  - `std::fma` exactly where the reference fuses;
  - FP contraction disabled for the file;
  - AArch64 float-to-int semantics (round-half-away, saturating).
- **[O] Comparison with the official addon** on identical synthetic inputs:
  - all integers, nulls, keys, errors and libm-free fields are
    bit-identical;
  - fields that go through log10, exp or hypot differ by at most 2.7e-15
    relative, because glibc and Apple's libm round differently in the last
    bit. Over 100k inputs on both machines the measured 1-ulp mismatches
    are: log10 3,325, exp 181, hypot 11,043; sin, cos and sqrt 0.
- **[D] Consequence.** A classification can differ only for a frame within
  about 1e-15 dB of a threshold.
- **[?] NaN samples.** Behaviour with NaN audio samples is not verified.

---

## 13. Screen capture statics (`src/screen/`)

Six statics on `Recorder`. They run synchronously on the JS thread and call
the optional log callback synchronously.

### 13.1 How Plaud uses them

- **[O] Permission checks are macOS-only.** On Linux and Windows,
  `checkScreenRecordingPermission()` returns `true` without calling any
  native static. Only macOS calls `checkScreenCapturePermissionFresh(probePath,
  cb)` and `requestScreenCapturePermission(cb)`
  (`createOrFocusWindow-DNFqWFOl.js:1080-1098`).
- **[O] `captureAllScreens`.** On non-mac platforms it passes `{rect:
  displays.map(d => d.physicalBounds)}` plus a log callback; macOS uses
  `displayId`. It accepts a Buffer or an Array of Buffers:
  `Buffer.isBuffer(result) ? [result] : result`
  (`createOrFocusWindow-DNFqWFOl.js:1064-1072`).
- **[O] `physicalBounds`** (`createOrFocusWindow-DNFqWFOl.js:1013-1028`,
  `:1043-1057`):
  - size = `round(bounds.size * scaleFactor)`;
  - origin = `round(dipToScreenPoint(bounds.origin + 2) - 2 * scaleFactor)`
    when `screen.dipToScreenPoint` is a function, else `round(bounds.origin
    * scaleFactor)`.
- **[O] Unused statics.** `getWindowAtCursor` and `extractTextFromImage`
  are never called from Plaud's main-process JS (grep of `main/`).
- **[O] `ScreenCapturePermissionStatus`**: Granted = 0, Denied = 1
  (`recordingService-Bn35UmTZ.js:156-160`).

### 13.2 Log callback

- **[O]** The callback receives one object whose keys are exactly `level`,
  `message`, in that order, with `this === undefined` [oracle].
- **[O]** The screen statics use the levels `info`, `warn` and `error`
  [mac and win disassembly; the Windows OCR stub logs `warn`]. The audio
  code uses `warning` instead.
- **[O]** An exception thrown by the callback inside the permission statics
  is swallowed, and the static still returns its value [oracle; the
  official also prints `Error in JS permission callback: <message>` to
  stderr, mac strings 11281]. **[D]** Linux swallows it silently, since the
  addon prints nothing to stdout or stderr.
- **[D]** In `captureScreenImage`, the first exception stops further
  logging and is rethrown, with the same value, after the capture
  [from the mac disassembly of the log lambda].
- **[O]** In a batch, log messages are prefixed `[i] ` [mac disassembly].

### 13.3 Permission statics

- **[O] Shapes.**
  - `checkScreenCapturePermission([cb])` returns a Number. A non-function
    argument is ignored [oracle: `check('x')` → number].
  - `checkScreenCapturePermissionFresh` accepts only `()`, `(cb)` or
    `(probePath: string, cb)`. Anything else throws `TypeError:
    checkScreenCapturePermissionFresh expects (callback) or (probePath,
    callback)` [oracle: `(undefined)`, `('/p')`, `(cb,'/p')`,
    `('/p',cb,3)`].
  - `requestScreenCapturePermission([cb])` returns a Boolean [mac
    disassembly].
- **[O] Official logs.** The first log is info `Starting screen capture
  permission check` [oracle]. On macOS a status line follows, for example
  `Screen capture permission status: Denied/Blocked` [oracle]. Windows logs
  `Screen capture permission is not required on Windows; assuming granted.`
  [win strings].
- **[D] Linux.**
  - When the capture setup succeeds and a backend is usable, it logs
    `Screen capture permission is not required on Linux; assuming granted.`
    and returns Granted;
  - otherwise it logs warn `No usable screen capture backend (<reason>).`
    and returns Denied;
  - `probePath` (the macOS TCC probe helper) is ignored.

### 13.4 `captureScreenImage([options], [cb])` grammar

Source: mac disassembly. Every message is present in the mac and win
strings, and the oracle confirms them [O].

**Call shape**

- No arguments, or argument 0 is `undefined`/`null` → one default request
  (the display under the cursor); argument 1 is ignored.
- Argument 0 is a function → it becomes the log callback, with one default
  request.
- Argument 0 is an object or array → argument 1 must be a function,
  `undefined` or `null`, else `TypeError: Second argument must be a
  callback function.`
- Any other argument 0 → `TypeError: First argument must be an options
  object or a callback function.`

**Options**

- An options array must be non-empty (`options array must be non-empty.`)
  and hold objects only (`options array elements must be objects.`). Each
  element is parsed as a request.
- An options object is checked in this order, for keys that are present
  and hold an array:
  1. `requests`: `requests array must be non-empty.`, `requests array
     elements must be objects.`;
  2. `displayId`: `… must be non-empty.`, `… elements must be numbers.`,
     `RangeError: displayId must be >= 0.`;
  3. `displayIndex`: the same three messages, with `RangeError:
     displayIndex must be >= 0.`;
  4. `windowId`: the same pattern, with `RangeError: windowId must be >
     0.`;
  5. `rect`: `rect array must be non-empty.`, `rect array elements must
     be objects.`
- Otherwise the object itself is parsed as one request.

**Request parsing**

- Precedence: `displayId` (number) > `displayIndex` (number) > `windowId`
  (number) > `rect` (object) > `mode` (`main`, `cursor` or `window`, else
  `TypeError: mode must be 'main', 'cursor', or 'window'.`) > the default
  cursor display.
- A key whose value has the wrong type is skipped silently [O oracle:
  `{displayId:'x', mode:'bad'}` → the mode TypeError; an array-valued
  `windowId` inside a request object falls back to the cursor display].
- A rect needs numeric `x`, `y`, `width` and `height`, else `TypeError:
  rect must have numeric {x, y, width, height}.`. `width <= 0` or `height
  <= 0` raises `RangeError: rect width/height must be > 0.`; NaN passes
  this check.
- Ids are converted to uint32 with saturation [O mac disassembly,
  `fcvtzu`].

**Results**

- One request returns a Buffer. A failure throws `Error: Failed to capture
  screen image.` [O win strings; the mac string appends a Screen Recording
  hint, mac strings 11162].
- Two or more requests return an Array of Buffers. The first failure
  throws `Failed to capture screen image (batch index N).`, with N 0-based
  [O].
- `No capture requests provided.` exists in both binaries but cannot be
  reached through parsing [O strings].

### 13.5 Linux capture model [D unless tagged]

**Backends**

- Wayland session, in this order:
  1. `grim`, when it is installed and the compositor advertises
     `zwlr_screencopy_manager_v1` or `ext_image_copy_capture_manager_v1`:
     `grim -o <output> -s <pixels/logical> -t ppm -`. It takes about 60 ms
     for a 3840x2400 output [O measured on an arm64 Hyprland machine];
  2. `org.freedesktop.portal.Screenshot`, non-interactive, about 1.1-1.9 s
     per call [O measured with xdg-desktop-portal-hyprland 1.4.1]. The
     portal's file is deleted only when it is under `$XDG_RUNTIME_DIR`,
     `/tmp`, `/var/tmp` or `$TMPDIR`.
- X11 session: `XGetImage` through `dlopen("libX11.so.6")`.
- X11 capture is refused inside a Wayland session, including an XWayland
  server without a reachable compositor.
- `PLAUD_SCREEN_BACKEND=grim|portal|x11` forces exactly one backend.

**Geometry and coordinate space**

- Geometry comes from a raw Wayland wire client that reads `wl_output` v4
  and `zxdg_output_manager_v1` v3. Hyprland or sway IPC is the fallback.
- XWayland RandR monitors come from `dlopen("libXrandr.so.2")`. They are
  matched to Wayland outputs by name, then by size, then by order.
- The process's coordinate space, in order of precedence:
  1. `PLAUD_SCREEN_COORDS=x11|wayland`;
  2. `--ozone-platform=` in `/proc/self/cmdline`;
  3. `--ozone-platform-hint=` in `/proc/self/cmdline`;
  4. `ELECTRON_OZONE_PLATFORM_HINT` (Electron < 38);
  5. Electron ≥ 38 → Wayland;
  6. otherwise X11 when `DISPLAY` is set.
- **[O] Measured with two side-by-side outputs on Hyprland.** Electron 36
  under XWayland (`--ozone-platform=x11`, `force_zero_scaling`) reports
  `display.id` equal to the RandR output XID. Its `physicalBounds` equal the
  XWayland RandR rects, `(0, 0, W1, H1)` and `(W1, 0, W2, H2)` in pixels.
  These differ from the Wayland logical layout.

**Rect semantics**

- A rect within ±2 px of a whole display returns that whole display,
  resized to the rect size. Both spaces are checked, the process's space
  first. The candidate display rects are:
  - the X11 RandR rect;
  - the Wayland rect `(round(lx·s), round(ly·s), pixelW, pixelH)`;
  - the Electron-Wayland `physicalBounds`, assuming `dipToScreenPoint` is
    the identity on Wayland, as Electron's documentation states.
- Any other rect is handled as on Windows [O win disassembly and strings]:
  - the bounds are `floor(x)`, `floor(y)`, `ceil(x+w)`, `ceil(y+h)`;
  - non-finite values → `CaptureScreenRectPng: non-finite rect values.`;
  - the rect is intersected with the bounding box of the virtual screen.
    An empty result → `CaptureScreenRectPng: requested rect is outside the
    virtual screen.`;
  - the PNG has the clipped size, and areas that no display covers are
    black.
- In a batch, each output is captured once and the parts are resampled from
  it: area average when downscaling, bilinear when upscaling, exact row
  copies at 1:1. The tests check the composites pixel by pixel.

**Display requests**

- An out-of-range `displayIndex` logs `displayIndex out of range; falling
  back to primary display.` [O win strings]. The macOS counterpart is
  `Invalid displayIndex=` [O mac strings]; Linux follows Windows.
- An unknown `displayId` logs `CaptureDisplayPngById: treating displayId as
  displayIndex on Linux.`. If it is still out of range, the log reads
  `displayId not found; falling back to primary display.` These are the
  Windows strings, with Linux in place of Windows [O win strings].
- The main display is the one containing logical (0,0); in the X11 space
  the RandR primary wins. The cursor display comes from Hyprland
  `j/cursorpos`, sway's focused output or the X11 pointer; otherwise the
  main display is used and a warn is logged.

**Windows**

- Ids:
  - Hyprland: `windowId = parseInt(stableId, 16)`, captured with `grim -T
    <stableId>` [O works on Hyprland 0.56], else cropped from the screen;
  - sway: the con id. The window under the cursor is the focused window,
    because sway's IPC has no pointer position;
  - X11: the client XID found through EWMH `_NET_CLIENT_LIST_STACKING`.
- Windows smaller than 50 px are ignored (`Window under cursor is too
  small; ignoring.`) [O win disassembly `0x32` and strings].
- `getWindowAtCursor` takes only an optional callback (else `TypeError:
  getWindowAtCursor expects an optional callback`). It returns `null` or
  `{windowId, x, y, width, height, ownerName, windowName}` [O oracle
  shape]:
  - the bounds are in the process's rect space;
  - `ownerName` is the app class or app_id;
  - `windowName` is the title.
- The Hyprland stacking order is a heuristic: special > pinned >
  fullscreen > floating > tiled, then `focusHistoryID`.

**Other behaviour**

- `extractTextFromImage` validates its arguments (`TypeError: First
  argument must be a PNG Buffer.`, `TypeError: Second argument must be a
  callback function.`), logs warn `OCR is not implemented on Linux in this
  build.` and throws `Failed to extract text from image (Vision OCR).` The
  Windows build does the same: its stub logs warn `OCR is not implemented
  on Windows in this build.` and fails [O win strings and disassembly].
- **[O] Performance, measured on an arm64 Hyprland machine.**
  - PNG encoding uses zlib level 1 with the Up filter: 64-103 ms and
    2.5-4.5 MB per 3840x2400 frame. Level 6 with all filters takes 303-533
    ms.
  - A 4K display with grim takes 110-180 ms end to end; the portal path
    takes about 1.1 s per call.

---

## 14. Environment

| Variable | Effect |
|---|---|
| `PIPEWIRE_REMOTE`, `PIPEWIRE_RUNTIME_DIR`, `XDG_RUNTIME_DIR` | Standard libpipewire connection variables |
| `PLAUD_AEC_MODEL_PATH`, `PLAUD_ANS_MODEL_PATH` | Defaults for the Plaud 3A model paths (§4.2) |
| `PLAUD_PRE_APM_DUMP` | Extra debug dumps (§7.7) |
| `TMPDIR` | Directory of the debug dumps |
| `PLAUD_SCREEN_BACKEND=grim\|portal\|x11` | Forces one screen capture backend (§13.5) |
| `PLAUD_SCREEN_COORDS=x11\|wayland` | Overrides the detected coordinate space (§13.5) |
| `WAYLAND_DISPLAY`, `DISPLAY`, `HYPRLAND_INSTANCE_SIGNATURE`, `SWAYSOCK`, `XDG_SESSION_TYPE` | Select the screen backends and window lookup |

## 15. Deviations from macOS and Windows

| Deviation | Why |
|---|---|
| Invalid device values in `start()` and `checkSystemAudioPermission()` without a callback throw a TypeError or skip the logs; the official addon aborts the process | [D] An abort kills Electron's main process. Same messages, recoverable failure. |
| Exceptions thrown by JS callbacks are swallowed silently | [D] The official addon prints them to stderr; this addon prints nothing outside the callbacks. |
| AEC is decided by "can the captured output be heard" (Bluetooth, headphone form factor, headphone port → off), not by "built-in speaker" | [D] Linux has no "built-in" transport flag; a USB or HDMI speaker leaks into the microphone like a laptop speaker does (§7.3). |
| Unplugging a selected device moves the stream to the default and back on re-plug, without a `fatal` event | [D] PipeWire and WirePlumber do this natively; the recording has no gap. |
| `begin()` for a source not enabled at start adds a stream instead of restarting the recorder | [D] A PipeWire stream can be added without touching the running one. |
| The last Ogg pages carry EOS; up to 360 ms of silence is appended at stop | [D] A finished stream for players and validators; RFC 6716 forbids empty packets (§8). |
| `stop()` delivers the final pages before it returns | [D] The JS ends the file stream right after `stop()` (§5). |
| `restart()` retries for up to 3 s and then stops the recorder | [D] A PipeWire restart takes a few hundred ms; the official JS has no further retry for this path. |
| `extractTextFromImage` has no OCR | [O] The official Windows build has none either, and Plaud never calls it. |

## 16. Build, dependencies, tests

- **Build.** `npm install && npm run build` in `native/recorder`:
  - `scripts/build-deps.mjs` builds the vendored webrtc-audio-processing
    2.1 and abseil into `deps/` (git-ignored). Downloads go to
    `deps/cache` (`PLAUD_DEPS_CACHE`); with `PLAUD_DEPS_OFFLINE=1` it never
    downloads, for offline and packaging builds. It fetches meson 1.5.2
    only when the host meson is missing or older than 0.63 (Ubuntu 22.04
    ships 0.61);
  - cmake-js builds `build/Release/recorder_linux.node`.
- **Build dependencies.** A C++17 compiler, cmake, python3, ninja, meson
  ≥ 0.63 (or network access for the pinned one) and the
  development files of libpipewire-0.3, opus, ogg, dbus-1, libpng, zlib,
  X11 and Xrandr (headers only):
  - Arch: `pipewire opus libogg dbus libpng zlib libx11 libxrandr`;
  - Debian/Ubuntu: `libpipewire-0.3-dev libopus-dev libogg-dev libdbus-1-dev
    libpng-dev zlib1g-dev libx11-dev libxrandr-dev`;
  - Fedora: `pipewire-devel opus-devel libogg-devel dbus-devel
    libpng-devel zlib-devel libX11-devel libXrandr-devel`.
- **Runtime `DT_NEEDED`.** `libpipewire-0.3.so.0`, `libopus.so.0`,
  `libogg.so.0`, `libdbus-1.so.3`, `libpng16.so.16`, plus libstdc++, libm,
  libgcc_s, libc and the dynamic loader. There are no undefined X11
  symbols: libX11 and libXrandr are loaded with `dlopen` when needed. The
  only exported symbols are the two N-API entry points and libstdc++
  template instantiations. A screen capture in a Wayland session needs
  `grim` or an xdg-desktop-portal with the Screenshot interface.
- **`npm test`** (`node --test --test-concurrency=1 test/`):

  | File | What it checks |
  |---|---|
  | `api-surface.mjs` | Every call in the official JS exists; class surface, order, `length`, exports |
  | `validation.test.mjs` | Every error message, error class and return value against the oracle: idle methods, 39 `start()` cases, 40 `setAudioDevice()` cases, list shapes, permission logs, watcher and mute-monitor lifecycles |
  | `record.test.mjs` | 5 s mixed recording (Opus, 48 kHz, mono, duration; both test tones present, by tone power against an off-frequency bin); the Ogg page layout including EOS; PCM mode; already-running and unknown-device cases; background noise only |
  | `pause.test.mjs` | `pause`/`begin`/`fillSilence` timing in the encoded file; `pause(0)`/`(1)`; `setEncodingPaused`; `setApmEnabled`; `apm` event keys |
  | `default-switch.test.mjs` | Following default sink and source switches; pinned non-default and pinned default devices; `setAudioDevice` while recording; muted output → silence; mute monitor events |
  | `hotplug.test.mjs` | The watcher on plug and unplug; unplug of the selected devices → fallback → return on re-plug, without a gap |
  | `aec.test.mjs` | Processed vs. raw microphone energy with AEC (−27.2 dB measured, ≤ −10 dB required) and with a headphone-type sink, AEC off (−5.4 to −5.5 dB, ≥ 10 dB gap required); no `apm` events with backend `none` |
  | `pipewire-restart.test.mjs` | Daemon crash → one fatal → `restart()` from outside and from inside the callback; failed restart → `false` and EOS; no daemon at start; watcher and mute monitor reconnect |
  | `concurrency.test.mjs` | 4 s of `pause`/`begin` on every mode plus a `restart()` every 250 ms while audio flows: no ring consumed from a second thread, no fatal, both sources audible afterwards, one valid Ogg Opus stream with rising granules and EOS |
  | `aec-metrics.test.mjs` | 117 tests: the 67 oracle cases and the argument probes, against `expected.json` |
  | `screen.test.mjs` | Grammar and error messages, permission statics, composites pixel by pixel, rect semantics, display modes, window lookup, forced backends |

- **[D] Isolation.**
  - Every PipeWire test file starts a private PipeWire, WirePlumber and
    pipewire-pulse instance (`test/support/pw.mjs`
    `startPrivatePipeWire()`) in a temporary runtime, config and state
    directory, under `setpriv --pdeathsig`. WirePlumber's ALSA, Bluetooth,
    V4L2 and libcamera monitors are disabled.
  - Devices are null sinks and remapped sources made with `pactl`, and test
    tones are played with `pw-play` into those sinks only.
  - The tests therefore never open the machine's microphones or speakers,
    never change its defaults and leave nothing in its WirePlumber state.
  - Test tools: `pipewire`, `wireplumber`, `pipewire-pulse`, `pactl`,
    `pw-play`, `pw-cli`, `ffmpeg`, `ffprobe`, and optionally `setpriv`.
    Without them, the PipeWire tests are skipped with the reason.
  - The screen tests need a graphical session and skip their live cases
    without one.
- **[O] In the patched app.** The Linux build of Plaud Desktop on an arm64
  Hyprland machine loaded the addon and recorded with it on 2026-10-26
  (`~/.config/Plaud/logs/log-2026-10-26.log`): the JS logged `Recorder
  initialized sampleRate=48000 channels=1 container=ogg`, the recorder
  started with pinned uids, followed the smart microphone through
  `SetPreferredDevices(…)` and `Switching microphone to …`, and stopped
  with `Recorder fully stopped and resources released.`

## 17. Open risks and unknowns

- **[?] Older WirePlumber.** WirePlumber 0.4 (Ubuntu 22.04, Debian 12) may
  ignore `node.dont-move` or `target.object` for pinned streams.
  `node.target` is set as well, but the fallback and re-plug behaviour of
  §6.3 is only verified with WirePlumber 0.5.18 and PipeWire 1.6.9.
- **[D] Stop padding.** A recording that stops exactly on a page boundary
  gains 360 ms of silence (§8).
- **[O] libm differences.** `computeAecLightMetrics` differs from macOS at
  about 1e-15 relative in libm-based fields (§12.5).
- **[D] Screen capture setup.** It needs `grim` or a Screenshot portal.
  Electron should run under XWayland (`--ozone-platform=x11` on argv) or
  set `PLAUD_SCREEN_COORDS`, so that Plaud's `physicalBounds` and the
  addon agree on the coordinate space. Switches added through
  `app.commandLine.appendSwitch` are invisible to the addon.
- **[?] Untested environments.** The X11-session pixel path, sway, GNOME
  and KDE are untested (no such sessions on the build machine). GNOME may
  show a one-time screenshot permission dialog.
- **[D] Blocking restart.** `restart()` blocks the JS thread for up to 3 s
  while PipeWire is down.
- **[?] Official per-page event granularity.** Whether the official addon
  emits one `encoded` event per Ogg page or batches pages is not
  established. The JS only concatenates them, so either works.
