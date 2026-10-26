# Architecture

Plaud Desktop for Linux (unofficial) runs the official Plaud Desktop
application on Linux. The official application is an Electron 36.7.4 app
whose JavaScript bundle is the same on macOS and Windows [O: the
`out-global-online/` trees of the 1.4.1 macOS and Windows builds are
byte-identical]; everything platform-specific sits in two native addons and a
few Electron calls. This project supplies Linux versions of those pieces and
a build that assembles them with the official bundle on the user's machine.

Evidence tags: **[O]** observed (file and line in the official 1.4.1 bundle,
`out-global-online/main/` unless a path is given, or a measurement on a test
machine), **[D]** derived, **[?]** not verified. Patch-level evidence is in
[patches.md](patches.md); the native addons are documented in
[native-api/recorder.md](native-api/recorder.md) and
[native-api/audio-monitor.md](native-api/audio-monitor.md).

## Components

| Path | Role |
|---|---|
| `native/recorder` | `recorder_linux.node`: PipeWire microphone and system-audio capture, echo cancellation (WebRTC Audio Processing), Ogg Opus encoding, device lists and hot-plug, screenshots |
| `native/audio-monitor` | `audio_monitor_linux.node`: meeting detection from PipeWire stream activity |
| `patcher/` | Applies anchored patches to the official `app.asar`, checks the native API surface, stages updates (also used at run time by the updater) |
| `linux-runtime/` | JavaScript injected into the archive: loader, polyfills, updater with private storage of staged releases, tray, autostart, global shortcuts, credential storage, desktop integration, Hyprland placement and focus helpers |
| `scripts/` | `fetch-official`, `build`, `package`, `install`, `uninstall`, `dev-run`, `smoke-test`, CI helpers |
| `packaging/` | Launcher, ffmpeg wrapper, desktop entry, AppStream metadata, deb/rpm/pacman/AppImage templates, AUR `PKGBUILD` |

## Build

`npm run build` (`scripts/build.js`) produces `build/linux-<arch>/`:

1. Builds both native addons (`scripts/build-native.js`, which runs
   `npm ci && npm run build` in each `native/*` module) and records the
   exported API of each addon in `native-api.json`.
2. Downloads the official macOS arm64 zip named in `official.json` from
   Plaud's update server and verifies its SHA-512 against the value Plaud
   publishes in `latest-mac.yml` (`scripts/fetch-official.js`). Only
   `Plaud.app/Contents/Resources/` is extracted. The macOS archive is used
   because its `app.asar` is platform-independent [O: identical to the
   Windows one] and it is a plain zip archive, while the Windows build ships
   as an NSIS installer that needs 7-Zip to unpack [O `plaud-setup.exe`].
3. Downloads Electron 36.7.4 for Linux with `@electron/get` and verifies it
   against the SHA-256 pinned in `scripts/electron-checksums.json`. The
   binary is renamed to `plaud-linux-bin`; `default_app.asar` is removed.
4. Patches the official `app.asar` (see [patches.md](patches.md)) and writes
   it to `resources/app.asar`, together with the API surface of this release
   (`api-baseline.json`), which the updater later compares new releases
   against.
5. Copies the addons, the ffmpeg wrapper and the launcher, and writes
   `resources/plaud-linux/` (build information, native API, baseline,
   licence files, and a copy of `patcher/` and `linux-runtime/` for the
   updater).
6. Renders hicolor icons (16 to 512 px) from the official `icon.icns` and
   writes the desktop entry and AppStream metadata into
   `build/linux-<arch>/share/`.

The *runtime id* is a SHA-256 over the patcher, the runtime, the native API
description and the Electron version (`patcher/lib/runtime-id.js`). It ties
staged updates to the exact build that produced them.

Nothing produced by the build is committed or published: `build/`, `dist/`
and `.cache/` are ignored, and CI uploads no artifacts.

## Start-up

```
plaud-linux (launcher.sh)
  -> plaud-linux-bin [flags] (stock Electron)
     -> resources/app.asar, package.json "main" = linux-runtime/loader.js
        -> newest healthy staged release of this build, or the packaged one
           -> linux-runtime/main.js
              -> polyfills, credential store, global shortcuts, AppImage integration
              -> official main (out-global-online/main/index.js)
```

### Launcher

`packaging/launcher.sh` (installed as `plaud-linux`):

- chooses the display backend (next section);
- uses the setuid `chrome-sandbox` when it is installed setuid root
  (system packages), otherwise Chromium's user-namespace sandbox when
  unprivileged user namespaces work, otherwise starts with `--no-sandbox`
  and says so on standard error (Ubuntu 23.10+ restricts user namespaces
  with AppArmor; the deb package ships an AppArmor profile that allows them
  for the installed binary, AppImages and per-user installs on such systems
  start without the sandbox);
- appends one flag per line from
  `${XDG_CONFIG_HOME:-~/.config}/plaud-linux-flags.conf`;
- exports `PLAUD_LINUX_LAUNCHER`, the command autostart and relaunch use.

### Display backend

In a Wayland session with XWayland available the app runs through XWayland
(`--ozone-platform=x11`); without XWayland it runs as a Wayland client.
`PLAUD_LINUX_OZONE=wayland` (or `--ozone-platform=wayland` in the flags
file) selects Wayland explicitly. Reasons:

- The app places its own windows: the tray panel above the tray icon
  [O `index-C7esC3cC.js:485-513`], the recording widget at the top right of
  the work area [O `index-Bcbok0vz.js:14-19`], the meeting tip and the
  screenshot overlays [O `index-CxItPqQT.js:438`, `index-CNPhXPLL.js:33-46`].
  Wayland clients cannot position their windows; the compositor decides.
- On Wayland, `screen.getCursorScreenPoint()` blocks the main process
  indefinitely while the app has no visible window [O: Electron 36.7.4 on
  Hyprland 0.56]. The official app calls it from four places
  [O `index-Bcbok0vz.js:14`, `index-CMiydsIK.js:49`, `index-CxItPqQT.js:438`,
  `index-Dyd2kgkk.js:49`], for example when a meeting is detected while only
  the tray icon is shown. For Wayland mode `linux-runtime/polyfills.js`
  replaces it with a version that queries Chromium only while a window is
  visible.
- Always-on-top levels (`pop-up-menu`, `modal-panel`) used for the panel,
  widget and tips [O `index-C7esC3cC.js:728`, `index-CxItPqQT.js:506`] are
  honoured by window managers for X11 clients; Wayland has no protocol for
  them.

HiDPI under XWayland: Chromium takes the scale from GTK (`GDK_SCALE`)
[O: with `GDK_SCALE=2` and Hyprland's `xwayland:force_zero_scaling`,
Electron 36.7.4 reports `scaleFactor` 2 and `devicePixelRatio` 2 on X11].
XWayland has one scale for all outputs, so on mixed-DPI setups windows on
the lower-DPI output are drawn at the higher scale [O: the 320x198 meeting
tip measured 640x396 logical pixels on a scale-1 monitor next to a scale-2
panel]; Wayland mode does not have that limitation and may be preferred
there.

### Window placement and focus

Window managers honour the positions XWayland clients request for their
floating windows [O: Hyprland 0.56, test window requested at (1600, 900)
mapped at (1600, 900); `setPosition(300, 400)` moved it there]. The official
placement code then works as on Windows once it gets correct inputs:

- Tray panel: `tray.getBounds()` is empty on Linux; `linux-runtime/tray.js`
  derives it from the last tray click and the bar position (patches.md,
  `tray-bounds`). A click on the tray icon also closes an open panel
  (patches.md, `tray-context-menu`).
- Highlight panel: floats on tiling window managers through its window type
  (patches.md, `recording-highlight-floating`).

#### Hyprland

In Hyprland sessions (`HYPRLAND_INSTANCE_SIGNATURE` set)
`linux-runtime/hyprland.js`, installed by `polyfills.installAfterReady()`,
corrects three things for both ozone platforms:

- Pointer position. XWayland reports the pointer only while it is over an
  X11 window, so `screen.getCursorScreenPoint()` is stale while the pointer
  rests on the bar or a Wayland window [D: X11 pointer events reach XWayland
  only for its surfaces]. The app uses it to choose the display for the
  meeting tip, the recording widget and the highlight panel
  [O `index-CxItPqQT.js:438`, `index-Bcbok0vz.js:14`, `index-CMiydsIK.js:49`].
  The polyfill reads `hyprctl -j cursorpos`.
- Work areas. Hyprland publishes no `_NET_WORKAREA`, so Electron's work
  areas include the bars [O: the tray panel log showed
  `displayWorkArea={"x":0,"y":0,"width":1920,"height":1200}` with a 28 px bar
  reserved on the left]; popups placed at the top right of the work area
  [O `index-CxItPqQT.js:437-457`] would overlap a top bar. `getAllDisplays`,
  `getPrimaryDisplay`, `getDisplayNearestPoint` and `getDisplayMatching`
  return work areas without the edges `hyprctl -j monitors` lists as
  `reserved` ([left, top, right, bottom]); monitor data is cached for two
  seconds.
  Monitors are paired with Electron displays by pixel size, because XWayland
  lays outputs out in its own coordinate space [O: Hyprland logical DP-6 at
  (-320, -1080), Electron display at (1920, 0)]; points convert with the
  ratio of Hyprland's monitor scale to Electron's scale factor.
- Focus of popups shown inactive. The meeting tip, notification and
  generated-notes popups are shown with `showInactive()`
  [O `windowService-hKDTYNww.js:198-202`; `focusWhenShow` unset in
  `index-CxItPqQT.js`]. Chromium marks such windows with
  `_NET_WM_USER_TIME` 0; Hyprland focuses them anyway [O: the meeting tip
  logged a `focus` event at map time]. `showInactive()` therefore maps the
  window under the title `Plaud popup`, matched by a runtime window rule with
  `no_initial_focus`, and restores the title one second later [O: with the
  rule the tip logged no `focus` event and the open tray panel kept the
  focus]. The rule is added with `hyprctl eval` (Lua configuration, 0.55+),
  else `hyprctl keyword windowrule` (0.53, 0.54) or `windowrulev2` (older),
  and added again after configuration reloads, which drop runtime rules
  (`configreloaded` on Hyprland's event socket).

Omarchy's floating workspaces resize every window that opens to at most
1100x760, centred, unless its application has a float rule of its own
[O `/usr/share/omarchy/default/hypr/workspace-modes.lua:238-256`; the tray
panel was moved to (410, 220) and resized to 1100x760 when shown on such a
workspace]. The app sets the size of the tray panel and the meeting tip
again after showing them, which on the tip took several seconds of
resizing, and their positions are lost [O: 192 `resizeWindow` log lines for
the tip in six seconds]. A float rule for Plaud in
`~/.config/hypr/hyprland.lua` excludes the app from
that treatment [D: `o.window` records float rules in `o.floating_apps`,
which the workspace rule excludes, `helpers.lua:281-323`,
`workspace-modes.lua:209-236`]:

```lua
o.window("^Plaud$", { float = true })
```

### Loader and staged releases

`linux-runtime/loader.js` runs first. If the updater has staged a newer
official release produced by this build (same runtime id, newer version, not
marked bad) that passes the checks described in "Updates" (private
directories, every file matching the SHA-256 recorded in its `stage.json`),
the loader records a boot attempt and loads that archive's
`linux-runtime/main.js` instead. A staged release counts as healthy after 20
seconds of running or when it quits normally; after two unhealthy boot
attempts it is marked bad and the packaged release is used. A release that
fails the checks is skipped with a warning in the runtime log, and so is
everything staged when the data, versions or state directory fails them. In
development (`app.isPackaged === false`) staged releases are ignored.
`PLAUD_LINUX_NO_STAGED=1` or `--plaud-linux-packaged` forces the packaged
release.

### Linux initialisation (`linux-runtime/main.js`)

Before the official code is loaded:

- `polyfills.installEarly()` provides `systemPreferences.getMediaAccessStatus`
  and `askForMediaAccess`, which exist only on macOS and Windows; the app
  calls the former while loading a module [O: without it the main process
  throws at start]. Linux has no per-application microphone permission for
  unsandboxed applications, so the answer is `granted`; device access is
  governed by PipeWire.
- `password-store.configureBeforeReady()` selects the credential backend.
  The app encrypts its tokens with `safeStorage`
  [O `compatible-OR3mc4zn.js:561`, `605`]. Chromium chooses libsecret or
  KWallet only for GNOME/KDE-like `XDG_CURRENT_DESKTOP` values and otherwise
  uses an unencrypted store in which `safeStorage` reports encryption as
  unavailable, so signing in would fail [D: Chromium `os_crypt` backend
  selection]. The runtime selects libsecret or KWallet explicitly when the
  service is on the session bus. If encryption is still unavailable after
  `ready`, it enables Chromium's plain-text mode (Chrome's behaviour on the
  same systems) and shows one notification recommending a Secret Service.
- `global-shortcuts.install()` prepares the XDG GlobalShortcuts portal
  session in Wayland sessions.
- AppImages install their own desktop entry and URL scheme handler
  (`desktop-integration.js`), because sign-in returns to the app through a
  `plaud://` link [O `service-BbehAAI0.js:14-37`].
- `relaunch.js` makes `app.relaunch()` work from an AppImage, whose mount
  disappears with the exiting process.

After `ready`: the screen polyfills (Wayland, Hyprland), the credential
fallback, and a check that the system ffmpeg can perform the app's encoding
(next section).

## Platform services

| Feature | macOS / Windows | Linux |
|---|---|---|
| Microphone and system audio recording, AEC, encoding | `recorder_mac.node` / `recorder.node` [O `nativeRecorder-D-_1ie7u.js:50`] | `recorder_linux.node` on PipeWire |
| Meeting detection | `audio_monitor_*.node` [O `createOrFocusWindow-DNFqWFOl.js:33`] | `audio_monitor_linux.node` on PipeWire |
| No audio device | App shows an error and quits when both device lists are empty [O `recordingService-Bn35UmTZ.js:40-50`, `app-BMVDJmxZ.js:186`] | Same; PipeWire virtual devices count as devices |
| Upload encoding | Bundled `ffmpeg` in `process.resourcesPath` [O `uploadService-DWSrEGvx.js:98-102`] | Wrapper that runs the system ffmpeg (`PLAUD_FFMPEG` overrides) |
| Tray | Template images / ICO, right-click menu; panel placed at `tray.getBounds()` | StatusNotifierItem or XEmbed via Electron; PNG icons and `setContextMenu`; panel placed at the last tray click (patches) |
| Popups shown without focus | `showInactive()` | `showInactive()`; in Hyprland sessions under a `no_initial_focus` window rule |
| Launch at login | Login items / registry | XDG autostart entry (patches) |
| `plaud://` links | `open-url` / registry protocol handler | Desktop entry with `x-scheme-handler/plaud`; single-instance lock forwards the link to the running app [O `service-BbehAAI0.js:17-37`] |
| Global shortcuts | `globalShortcut` | `globalShortcut` plus the XDG GlobalShortcuts portal in Wayland sessions (patch) |
| Updates | electron-updater [O `misc-DYpkuuKJ.js:9`] | `linux-runtime/updater.js` (patch, next section) |
| Credential storage | Keychain / DPAPI via `safeStorage` | libsecret or KWallet, plain-text fallback |
| Notifications | Native | Electron notifications over D-Bus (`org.freedesktop.Notifications`) |
| Microphone permission | TCC prompt / Windows privacy settings | Always granted (polyfill); no per-app permission exists |
| Screenshots and screen capture permission | `CGDisplayCreateImage`/`CGWindowListCreateImage` with the Screen Recording permission [O `recorder_mac.node` strings] / GDI `BitBlt`, `PrintWindow` [O `recorder.node` strings] | Recorder addon: grim (wlr-screencopy) or the XDG Screenshot portal in Wayland sessions, `XGetImage` in X11 sessions; see the recorder documentation |

### ffmpeg

The upload service runs `<resourcesPath>/ffmpeg` to encode PCM to Ogg Opus
and to read durations [O `uploadService-DWSrEGvx.js:98-102`]. Builds ship
`packaging/ffmpeg.sh` there: it runs `$PLAUD_FFMPEG` or the first `ffmpeg`
on `PATH` other than itself. Using the distribution's ffmpeg keeps codec
licensing and security updates with the distribution. At start the runtime
runs the app's exact encoding pipeline once on generated audio
(`linux-runtime/ffmpeg.js`) and notifies the user if it fails (for example,
ffmpeg built without libopus).

## Updates

The official feed has no Linux files [O: `latest-mac.yml` and `latest.yml`
only]. The Linux updater:

1. reads `latest-mac.yml` from the URL the app configures, and treats a newer
   version as an update (pre-releases only when the app allows them);
2. if that version is already staged and passes the checks below, reports
   it as downloaded; if it was refused before by this build, reports no update;
3. downloads the arm64 zip into `~/.cache/plaud-linux/downloads/`, with
   progress events, and verifies size and SHA-512;
4. runs `patcher/cli.js stage` with `ELECTRON_RUN_AS_NODE=1` in a child
   process: extract the resources, check the native API surface against this
   build's addons and baseline, apply the patches, inject this build's
   runtime, and atomically move the result to
   `~/.local/share/plaud-linux/versions/<version>/`;
5. on success emits `update-downloaded`. The app installs it as on the
   other platforms, after the user chooses to update or automatically while
   idle [O `misc-DYpkuuKJ.js:181-191`, `316-337`], by calling
   `quitAndInstall()`, which relaunches into the staged release. On refusal
   (patch or API mismatch) the updater records the version in
   `~/.local/state/plaud-linux/refused-updates.json`, shows one
   notification, and reports no update.

The installed files are never modified, so system packages remain owned by
the package manager. Staged releases from other builds and obsolete versions
are removed a minute after start.

### Trust in staged releases

A staged release is code the app runs, written at run time into the user's
home directory, so it is only accepted from places no other user can change
(`linux-runtime/secure-fs.js`) [D]:

- `~/.local/share/plaud-linux` (with `versions/` and each `<version>/`),
  `~/.cache/plaud-linux` (with `downloads/`) and `~/.local/state/plaud-linux`
  are created with mode 0700, set with `fchmod` after creation so the umask
  plays no part. `scripts/install` creates and tightens the data directory
  the same way.
- Before the updater writes to or the loader reads from them, each of these
  directories is checked with `lstat`: a directory, not a symbolic link,
  owned by the current user, not writable by its group or other users. Files
  are opened with `O_NOFOLLOW` and checked the same way with `fstat`.
- Files are written through exclusive temporary files
  (`O_CREAT | O_EXCL | O_NOFOLLOW`, mode 0600, random name) renamed into
  place; downloads get a new exclusive file in the private `downloads/`.
- The staging child writes `stage.json` last, after the archive, with the
  SHA-256 of every file of the release (`app.asar`, and
  `app.asar.unpacked/...` when a release has one). The loader hashes the
  files again before it selects the release (about 40 MB for Plaud 1.4.1)
  and requires the same set of files with the same checksums.
- A failed check never stops the app: the updater reports an error to the
  official update service (`ERR_UNSAFE_PATH`) and writes nothing, the loader
  logs the reason and starts the packaged release. The directories are not
  repaired automatically; `chmod 700` on the directory the log names, or
  removing it, restores updates.

The base directories themselves (`XDG_DATA_HOME`, `XDG_CACHE_HOME`,
`XDG_STATE_HOME`) belong to the user's setup and may be symbolic links; they
are created with mode 0700 when missing, as the XDG Base Directory
specification asks, but not checked.

A test (`test/integration/updater.test.js`) runs the update flow in a
headless Electron against a local feed with releases built from the official
archive: a supported release is staged with umask 0000 into mode 0700
directories with a correct `stage.json` and selected for boot, a release
with a changed anchor is refused once, a checksum mismatch is an error, a
cache directory replaced by a symbolic link is refused before anything is
downloaded, and a staged archive changed afterwards is not selected.
`test/runtime.test.js` covers the checks one by one (umask 0000 and 0002,
group- or world-writable, symbolic-link and root-owned directories, linked,
shared or FIFO files, modified, missing and added files, releases without
checksums).

## Files

| Path | Content |
|---|---|
| `~/.config/Plaud/` | Official app data (settings, tokens, recordings cache, `logs/`) [O: `userData` of the app] |
| `~/.config/Plaud/logs/linux-runtime-YYYY-MM-DD.log` | Runtime log (JSON lines; `PLAUD_LINUX_LOG_LEVEL=debug` for more) |
| `~/.config/plaud-linux-flags.conf` | Extra Electron/Chromium flags, one per line |
| `~/.config/autostart/io.github.trukhinyuri.PlaudLinux.desktop` | Launch at login |
| `~/.local/share/plaud-linux/versions/` | Staged official releases (private, mode 0700) |
| `~/.local/state/plaud-linux/` | Boot attempts, bad and refused versions, shown notices, portal shortcut state (private) |
| `~/.cache/plaud-linux/downloads/` | Update downloads in progress (private) |

## Packaging

| Format | Layout | Notes |
|---|---|---|
| Per-user (`scripts/install`) | `~/.local/share/plaud-linux/app`, `~/.local/bin/plaud-linux` | No setuid sandbox; desktop entry with an absolute `Exec` |
| AppImage | type2 runtime (pinned SHA-256, `scripts/appimage-runtime.json`) + zstd SquashFS | Installs its desktop entry on first start; relaunch helper |
| deb | `/opt/plaud-linux`, `/usr/bin/plaud-linux` | setuid `chrome-sandbox`; AppArmor `userns` profile on AppArmor 4 systems |
| rpm | same | dependencies from sonames (AutoReq), bundled Electron libraries excluded |
| pacman | same | `packaging/arch/PKGBUILD` builds from source for the AUR |

All formats depend on the system's PipeWire client library, ffmpeg and
xdg-utils; the addons link libpipewire, libopus, libogg, libdbus, libpng and
zlib dynamically and load libX11/libXrandr at run time. `npm run package`
checks every addon dependency against that list before packaging.

## Testing

| Suite | Covers |
|---|---|
| `test/patcher.test.js` | Every anchor applies exactly once to the pinned release and lies within its evidence lines; patched files compile; double patching and API mismatches are refused |
| `test/archive.test.js` | asar reader/writer against `@electron/asar`; zip extraction against Python's `zipfile` (modes, symlinks, CRC errors) |
| `test/feed.test.js`, `test/surface.test.js`, `test/icons.test.js` | Feed parsing and download verification, API surface extraction, ICO/ICNS decoding and PNG output |
| `test/runtime.test.js` | Autostart entries, staging and boot selection, private files and directories, shortcuts, polyfills, tray placement, credential backend selection |
| `test/dbus.test.js` | D-Bus marshalling and a live session bus |
| `test/packaging.test.js` | Launcher, ffmpeg wrapper, desktop entry and AppStream validation, package versions, skipped-test detection for the native suites, per-user install/upgrade/uninstall |
| `test/integration/updater.test.js` | Update flow in headless Electron, including the private-directory checks |
| `scripts/smoke-test.js` | Starts a built app and waits until both addons are loaded, then quits it through the inspector |

CI (`.github/workflows/ci.yml`) runs all of the above on x86_64 and aarch64
Ubuntu 24.04 inside a private PipeWire session with virtual devices
(`scripts/ci/session.sh`), plus the native addon suites, which start their
own PipeWire, WirePlumber and pipewire-pulse and run in an X11 session on
Xvfb with a window manager (`scripts/ci/x11-desktop.sh`); a skipped native
test fails the job (`npm run test:native -- --fail-on-skip`). It builds the AppImage,
deb and rpm, installs the deb and starts it, starts the AppImage, and builds
and installs the pacman package in an Arch Linux container. A daily run
checks the newest official release against the patches.
