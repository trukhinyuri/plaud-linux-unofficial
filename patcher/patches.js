'use strict';

// Declarative source patches applied to the official main-process bundle.
//
// Rules (enforced by patcher/index.js):
//  - `file` is a path inside app.asar whose basename may contain `*`
//    wildcards (Vite chunk hashes change every release).
//  - `anchor` must occur exactly once across all files matching `file`;
//    otherwise patching fails and the build (or in-app update) is refused.
//  - For every `id`, exactly one entry whose `versions` range contains the
//    upstream version must exist.
//  - Linux code lives in linux-runtime/ inside the patched archive and is
//    reached with require("../../linux-runtime/<module>.js") from
//    out-global-online/main/.
//
// Evidence references point at the official v1.4.1 bundle and are explained
// in docs/patches.md.

const RUNTIME = '../../linux-runtime';

module.exports = [
  {
    id: 'native-platform-dir',
    file: 'out-global-online/main/nativeRecorder-*.js',
    versions: '>=1.4.1',
    anchor: 'process.platform === "darwin" ? "darwin" : "win32-x64"',
    replacement:
      'process.platform === "darwin" ? "darwin" : process.platform === "linux" ? `linux-${process.arch}` : "win32-x64"',
    rationale: 'Development builds load prebuilt addons from prebuilds/<platform dir>; Linux uses linux-<arch>.',
    evidence: 'nativeRecorder-D-_1ie7u.js:37',
  },
  {
    id: 'recorder-addon-name',
    file: 'out-global-online/main/nativeRecorder-*.js',
    versions: '>=1.4.1',
    anchor: '"recorder_mac.node" : "recorder.node"',
    replacement: '"recorder_mac.node" : process.platform === "linux" ? "recorder_linux.node" : "recorder.node"',
    rationale: 'Selects the Linux recorder addon (PipeWire capture, Opus encoding) shipped in resources/.',
    evidence: 'nativeRecorder-D-_1ie7u.js:50',
  },
  {
    id: 'audio-monitor-addon-name',
    file: 'out-global-online/main/createOrFocusWindow-*.js',
    versions: '>=1.4.1',
    anchor: '"audio_monitor_mac.node" : "audio_monitor_win.node"',
    replacement:
      '"audio_monitor_mac.node" : process.platform === "linux" ? "audio_monitor_linux.node" : "audio_monitor_win.node"',
    rationale: 'Selects the Linux meeting-detection addon (PipeWire stream monitoring) shipped in resources/.',
    evidence: 'createOrFocusWindow-DNFqWFOl.js:33',
  },
  {
    id: 'tray-icon-path',
    file: 'out-global-online/main/index-*.js',
    versions: '>=1.4.1',
    anchor: 'return path.join(__dirname, `../../resources/tray/${iconName}`);',
    replacement:
      `return process.platform === "linux" ? require("${RUNTIME}/tray.js").iconPath(type) : path.join(__dirname, \`../../resources/tray/\${iconName}\`);`,
    rationale: 'Non-macOS builds use .ico tray icons, which nativeImage only decodes on Windows; Linux uses PNGs rendered from the same ICO files at build time.',
    evidence: 'index-C7esC3cC.js:131-153',
  },
  {
    id: 'tray-context-menu',
    file: 'out-global-online/main/index-*.js',
    versions: '>=1.4.1',
    anchor: 'tray.setToolTip(',
    replacement: `process.platform === "linux" && require("${RUNTIME}/tray.js").attachContextMenu(tray, getMenu);\n  tray.setToolTip(`,
    rationale: 'The menu is shown from the "right-click" event via popUpContextMenu, neither of which exists for StatusNotifierItem trays; Linux exports the same menu with setContextMenu and keeps it current.',
    evidence: 'index-C7esC3cC.js:408-413',
  },
  {
    id: 'tray-bounds',
    file: 'out-global-online/main/index-*.js',
    versions: '>=1.4.1',
    anchor: 'const bounds = tray?.getBounds();',
    replacement: `const bounds = process.platform === "linux" ? require("${RUNTIME}/tray.js").getBounds(tray) : tray?.getBounds();`,
    rationale: 'Tray.getBounds() is always empty on Linux; the tray panel is anchored to an estimated tray position instead of the screen origin.',
    evidence: 'index-C7esC3cC.js:232-235, 485-513',
  },
  {
    id: 'autostart-set',
    file: 'out-global-online/main/compatible-*.js',
    versions: '>=1.4.1',
    anchor: 'async function setAutoLaunch(enabled) {',
    replacement: `async function setAutoLaunch(enabled) {\n  if (process.platform === "linux") return require("${RUNTIME}/autostart.js").setAutoLaunch(enabled);`,
    rationale: 'Non-macOS auto-launch writes the Windows registry with reg.exe; Linux uses an XDG autostart entry.',
    evidence: 'compatible-OR3mc4zn.js:1498-1554',
  },
  {
    id: 'autostart-get',
    file: 'out-global-online/main/compatible-*.js',
    versions: '>=1.4.1',
    anchor: 'function getAutoLaunch() {',
    replacement: `function getAutoLaunch() {\n  if (process.platform === "linux") return require("${RUNTIME}/autostart.js").getAutoLaunch();`,
    rationale: 'getLoginItemSettings() carries no auto-launch state on Linux.',
    evidence: 'compatible-OR3mc4zn.js:1555-1566',
  },
  {
    id: 'autostart-migrate',
    file: 'out-global-online/main/compatible-*.js',
    versions: '>=1.4.1',
    anchor: 'async function migrateAutoLaunchArgs() {',
    replacement: `async function migrateAutoLaunchArgs() {\n  if (process.platform === "linux") return require("${RUNTIME}/autostart.js").migrateAutoLaunch();`,
    rationale: 'Keeps an enabled autostart entry pointing at the current launcher (for example after an AppImage moved).',
    evidence: 'compatible-OR3mc4zn.js:1573-1580',
  },
  {
    id: 'updater',
    file: 'out-global-online/main/misc-*.js',
    versions: '>=1.4.1',
    anchor: 'const electronUpdater = require("electron-updater");',
    replacement: `const electronUpdater = process.platform === "linux" ? require("${RUNTIME}/updater.js") : require("electron-updater");`,
    rationale: 'electron-updater has no Linux target for this app; the Linux updater re-patches official macOS releases and stages them for the next start.',
    evidence: 'misc-DYpkuuKJ.js:9, 262-387',
  },
  {
    id: 'analytics-os',
    file: 'out-global-online/main/compatible-*.js',
    versions: '>=1.4.1',
    anchor: 'os: isMac ? "macOS" : "windows",',
    replacement: 'os: isMac ? "macOS" : process.platform === "linux" ? "linux" : "windows",',
    rationale: 'Reports the real operating system instead of "windows".',
    evidence: 'compatible-OR3mc4zn.js:1078',
  },
  {
    id: 'global-shortcuts',
    file: 'out-global-online/main/shortcutService-*.js',
    versions: '>=1.4.1',
    anchor: 'return electron.globalShortcut.register(accelerator, handler);',
    replacement: `return process.platform === "linux" ? require("${RUNTIME}/global-shortcuts.js").register(actionId, accelerator, handler) : electron.globalShortcut.register(accelerator, handler);`,
    rationale: 'In Wayland sessions X11 key grabs only see XWayland windows; Linux registers recording shortcuts through the XDG GlobalShortcuts portal when available and falls back to Electron otherwise.',
    evidence: 'shortcutService-BSPv4w4s.js:163-212',
  },
  {
    id: 'recording-highlight-floating',
    file: 'out-global-online/main/index-*.js',
    versions: '>=1.4.1',
    anchor: 'minWidth: MIN_WIDTH,',
    replacement: '...process.platform === "linux" ? { type: "toolbar" } : {},\n      minWidth: MIN_WIDTH,',
    rationale: 'The highlight panel is a resizable frameless window whose "panel" type exists only on macOS; tiling window managers (Hyprland, Sway, i3) tile it like a document window. The toolbar window type makes them float it, as the panel floats on macOS and Windows.',
    evidence: 'index-CMiydsIK.js:55-75',
  },
];
