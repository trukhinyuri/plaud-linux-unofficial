# Patches to the official application

This document lists every change the patcher makes to the official Plaud
Desktop `app.asar`, and why. The declarations live in
[`patcher/patches.js`](../patcher/patches.js); the archive-level steps in
[`patcher/index.js`](../patcher/index.js).

Evidence tags: **[O]** observed in the official 1.4.1 code or binaries (file
and line given; file names are those inside `app.asar`,
`out-global-online/main/`), **[D]** derived from observations, **[?]** not
verified.

## How a patch is applied

Each patch names a file pattern inside the archive (the bundle's file names
carry a content hash, for example `nativeRecorder-D-_1ie7u.js`, so patterns
use `*`), a semver range of official versions, an exact `anchor` string and
its `replacement`. Rules, enforced by `applyPatches`:

- the pattern must match exactly one file, and the anchor must occur in it
  exactly once; otherwise the release is refused with `ANCHOR_NOT_FOUND` or
  `ANCHOR_AMBIGUOUS`;
- a release outside a patch's version range is refused with
  `UNSUPPORTED_VERSION`;
- every replacement keeps the original behaviour for macOS and Windows and
  only adds a `process.platform === "linux"` branch, so a patched file can be
  compared with the original line by line;
- patches that insert code before an anchor keep the anchor in the
  replacement (`autostart-*`, `tray-context-menu`).

The anchor test (`node patcher/cli.js check --resources <Resources dir>` and
`test/patcher.test.js`) applies all patches in memory to a release and fails
if any rule is violated. The test also checks that each anchor sits within
the line range given as evidence below.

## Archive-level changes

| Step | What it does | Why |
|---|---|---|
| Tray icons | Renders `resources/tray/linux/Icon-{off,on,ppc,update}.png` (64 px) from the official `resources/tray/Icon-*.ico` | Electron decodes ICO files only on Windows [D]; the StatusNotifierItem tray needs PNG |
| Native content | Removes `node_modules/electron-app-universal-protocol-client/prebuilds` and stores all formerly unpacked files inside the archive; refuses any other `.node`/`.dylib` | The prebuild is a macOS binary, loaded only on macOS [O `electron-app-universal-protocol-client/dist/index.js:73-136`]; on Linux the package uses `setAsDefaultProtocolClient` [O same file, 172-180] |
| Runtime | Adds `linux-runtime/` (this repository's runtime) to the archive | Loader, polyfills and Linux services; see [architecture.md](architecture.md) |
| Manifest | Writes `linux-runtime/patch-manifest.json` (upstream version and main, runtime id, Electron version, per-patch SHA-256 before/after) | Detects double patching (`ALREADY_PATCHED`) and documents the result |
| package.json | `main` becomes `linux-runtime/loader.js`; adds `desktopName` (`io.github.trukhinyuri.PlaudLinux.desktop`) and `plaudLinux { upstreamMain, upstreamVersion, runtimeId }` | The loader installs the runtime before the official main runs; `desktopName` makes Electron set `CHROME_DESKTOP`, which `setAsDefaultProtocolClient` uses on Linux [D] |

Before any of this, the release must pass the native API check
(`patcher/lib/api-surface.js`): every native call the official main bundle
makes through `Recorder.*`, `recorder.*` and the meeting detector must be
exported by the Linux addons, no new identifier may appear in both the
official addons and the main bundle relative to the release this build was
verified against, and no unknown addon may ship. A failure is reported as
`API_SURFACE` and the release is refused.

## Patch list (official 1.4.1)

| Id | File | Purpose |
|---|---|---|
| `native-platform-dir` | `nativeRecorder-*.js` | Development addon directory `linux-<arch>` |
| `recorder-addon-name` | `nativeRecorder-*.js` | Load `recorder_linux.node` |
| `audio-monitor-addon-name` | `createOrFocusWindow-*.js` | Load `audio_monitor_linux.node` |
| `tray-icon-path` | `index-*.js` | PNG tray icons |
| `tray-context-menu` | `index-*.js` | Tray menu through `setContextMenu` |
| `tray-bounds` | `index-*.js` | Tray position from the last tray click |
| `autostart-set` | `compatible-*.js` | XDG autostart instead of the Windows registry |
| `autostart-get` | `compatible-*.js` | Read the XDG autostart state |
| `autostart-migrate` | `compatible-*.js` | Keep the autostart entry pointing at the current launcher |
| `updater` | `misc-*.js` | Linux updater instead of electron-updater |
| `analytics-os` | `compatible-*.js` | Report `linux` as the operating system |
| `global-shortcuts` | `shortcutService-*.js` | Portal-based global shortcuts in Wayland sessions |
| `recording-highlight-floating` | `index-*.js` | Highlight panel floats on tiling window managers |

### native-platform-dir

- Official: development builds load prebuilt addons from
  `packages/native-recorder/prebuilds/<dir>` with `<dir>` either `darwin` or
  `win32-x64` [O `nativeRecorder-D-_1ie7u.js:37`].
- Linux: `linux-${process.arch}`, matching `scripts/dev-run.js`, which links
  the freshly built addons there. Packaged builds do not use this path: they
  load addons from `process.resourcesPath` [O `nativeRecorder-D-_1ie7u.js:46-50`].

### recorder-addon-name

- Official: `recorder_mac.node` on macOS, otherwise `recorder.node`
  [O `nativeRecorder-D-_1ie7u.js:50`].
- Linux: `recorder_linux.node` (PipeWire capture, echo cancellation, Ogg Opus
  encoding, screenshots), built from `native/recorder`.

### audio-monitor-addon-name

- Official: `audio_monitor_mac.node` on macOS, otherwise
  `audio_monitor_win.node` [O `createOrFocusWindow-DNFqWFOl.js:33`].
- Linux: `audio_monitor_linux.node` (meeting detection from PipeWire streams),
  built from `native/audio-monitor`.

### tray-icon-path

- Official: macOS uses template PNGs; other platforms use
  `resources/tray/Icon-*.ico` [O `index-C7esC3cC.js:131-153`].
- Linux: `linux-runtime/tray.js` returns the PNGs rendered by the patcher.
  Electron's `nativeImage` reads ICO only on Windows [D: an ICO path yields an
  empty image on Linux, so the tray would be invisible].

### tray-context-menu

- Official: the menu is built by `getMenu()` and shown from the tray's
  `right-click` event with `popUpContextMenu` [O `index-C7esC3cC.js:408-413`].
- Linux: StatusNotifierItem trays (KDE, Waybar, GNOME with the AppIndicator
  extension) emit neither `right-click` nor support `popUpContextMenu`
  [D: Electron documents both as macOS/Windows-only]. The patch inserts
  `attachContextMenu(tray, getMenu)` before `tray.setToolTip(`, which sets the
  same menu with `setContextMenu` and rebuilds it when the menu's inputs
  change, so the host shows it natively.
- The same call registers a `click` listener ahead of the official one
  [O: the official handler is registered later in `initTray`,
  `index-C7esC3cC.js:419`]. It records the pointer position for `tray-bounds`
  and makes a click on the tray icon close an open tray panel. On Windows the
  click takes the focus from the panel, which hides it on `blur`, and the
  official handler leaves the panel hidden when the blur happened less than
  300 ms earlier [O `index-C7esC3cC.js:347-355`, `752-778`]. Clicking a
  StatusNotifierItem moves no focus [O: on Hyprland with the Quickshell tray,
  `Activate` on the item left the panel focused], so the official handler
  would show the open panel again; the listener emits the `blur` event the
  panel would have received [O: second `Activate` logged
  `handleRecordingMainWindow shouldHideWindow` and the panel hid].

### tray-bounds

- Official: the tray panel is placed relative to `tray.getBounds()`
  [O `index-C7esC3cC.js:232-235`, `485-513`]: below the tray icon in the upper
  half of the display, above it otherwise, clamped to the work area.
- Linux: `getBounds()` always returns an empty rectangle [D: Electron
  documents it for macOS and Windows only], which would place the panel at
  the screen origin. `linux-runtime/tray.js` returns a 24 px box at the
  pointer position of the last tray click, moved to the inner edge of the
  panel (bar) that reserves space on that side of the work area, so the
  official placement puts the tray panel beside the bar. The pointer
  position is known in X11 sessions and, through `hyprctl`, in Hyprland
  sessions (architecture.md, "Hyprland"); elsewhere, and before the first
  click, the box sits in a work-area corner on the side of the bar (the
  bottom corner for vertical bars), or the top-right corner without a bar.
  [O on Hyprland, Omarchy bar on the left: tray click at (96, 659) logged
  `originalTrayBounds={"x":28,"y":647,...} newPosition={"x":28,"y":562}`.]

### autostart-set, autostart-get, autostart-migrate

- Official: on Windows, auto-launch writes and reads the registry with
  `reg.exe` [O `compatible-OR3mc4zn.js:1498-1554`, `1555-1566`] and rewrites
  stale arguments at start [O `compatible-OR3mc4zn.js:1573-1580`]; macOS uses
  `setLoginItemSettings`. The app enables auto-launch by default on first run
  (`setupDefaultAutoLaunch`) [O `index-C7esC3cC.js:715`].
- Linux: `linux-runtime/autostart.js` writes
  `$XDG_CONFIG_HOME/autostart/io.github.trukhinyuri.PlaudLinux.desktop` with
  the current launcher (AppImage path, packaged launcher, or `electron <app>`
  in development), reads it back for the settings switch, and rewrites it
  when the launcher moved. The official first-run default is kept.

### updater

- Official: `electron-updater` with the `latest-mac.yml`/`latest.yml` feeds
  [O `misc-DYpkuuKJ.js:9`, `262-387`]. The app sets `channel`,
  `allowDowngrade`, `allowPrerelease` and the feed (`setFeedURL`), reads
  `currentVersion` [O `misc-DYpkuuKJ.js:282-302`], calls `checkForUpdates`
  [O `misc-DYpkuuKJ.js:264`] and `quitAndInstall(true, true)`
  [O `misc-DYpkuuKJ.js:191`, `336`], and listens to `checking-for-update`,
  `update-available`, `update-downloaded`, `update-not-available`, `error`
  and `download-progress` [O `misc-DYpkuuKJ.js:341-381`]. It never calls
  `downloadUpdate` and leaves `autoDownload` at electron-updater's default
  (true), so the check itself downloads [O: no other `autoUpdater` member is
  used in `out-global-online/main`].
- Linux: `linux-runtime/updater.js` implements exactly that interface
  (plus `forceDevUpdateConfig` for the integration test). `checkForUpdates`
  does the whole update: it reads the macOS feed, downloads the arm64 zip
  (the platform-independent `app.asar` is identical across platforms
  [D: 1.4.1 mac and win archives compared]), verifies its SHA-512, and runs
  `patcher/cli.js stage` in a child process, which applies these same
  patches and the API check and stages the result in a private directory
  with a `stage.json` manifest of SHA-256 checksums (see architecture.md,
  "Updates"). A release that fails the patches is refused once, remembered
  in `~/.local/state/plaud-linux/refused-updates.json`, and the app reports
  "no update" so it keeps working; the user gets one desktop notification.
  Staged releases boot on the next start.

### analytics-os

- Official: analytics report `os: isMac ? "macOS" : "windows"`
  [O `compatible-OR3mc4zn.js:1078`].
- Linux: reports `linux`, so Linux usage is not counted as Windows.

### global-shortcuts

- Official: recording shortcuts are registered with
  `electron.globalShortcut.register` [O `shortcutService-BSPv4w4s.js:163-212`].
- Linux: `linux-runtime/global-shortcuts.js`. In Wayland sessions the
  shortcuts are also bound through the XDG GlobalShortcuts portal
  (`org.freedesktop.portal.GlobalShortcuts`), because X11 key grabs made by
  an XWayland client only fire while an XWayland window has focus [D]. The
  portal's `Activated` signal calls the same handler. Electron registration
  is kept as well, so shortcuts work on X11 sessions and with portals that
  lack the interface. Each handler fires at most once per key press when
  both paths deliver it. Hyprland's portal binds shortcuts only through the
  compositor configuration; see the README.

### recording-highlight-floating

- Official: the recording highlight panel is a frameless, resizable,
  always-on-top window of type `panel` [O `index-CMiydsIK.js:55-75`]; the type
  exists only on macOS, and on Windows the window floats like every window.
- Linux: tiling window managers tile resizable windows of the normal type
  [O: on Hyprland a frameless resizable 420x360 test window was tiled,
  `floating: false`], so the highlight panel would reflow the user's windows
  while recording. The patch adds `type: "toolbar"` on Linux
  (`_NET_WM_WINDOW_TYPE_TOOLBAR`), which tiling window managers float
  [O: the same test window with `type: "toolbar"` was floating on Hyprland].
  The window stays resizable and focusable.

## Updating for a new official release

1. `node scripts/fetch-official.js --latest` downloads and verifies the
   latest release into `.cache/official/<version>/`.
2. `node patcher/cli.js check --resources .cache/official/<version>/Resources`
   reports each patch as applied or the exact failure.
3. For a failed anchor, read the new code, adjust `anchor`/`replacement`,
   update `evidence` with the new file and line numbers, and widen
   `versions` only after the new code was read.
4. Run `npm test` with `PLAUD_OFFICIAL_RESOURCES` pointing at the new
   Resources directory, then update `official.json` (version, file, SHA-512
   and size from the feed).

Installed apps do this check themselves for every release the official feed
offers; a release that needs a code change here is refused until a new
build of this project is installed.
