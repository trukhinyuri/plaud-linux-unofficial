'use strict';

// linux-runtime modules under plain Node, with a stand-in for `electron` and
// every XDG directory pointed at a temporary tree.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'plaud-runtime-'));
for (const [name, dir] of Object.entries({ XDG_CONFIG_HOME: 'config', XDG_DATA_HOME: 'data', XDG_STATE_HOME: 'state', XDG_CACHE_HOME: 'cache' })) {
  process.env[name] = path.join(ROOT, dir);
}
test.after(() => fs.rmSync(ROOT, { recursive: true, force: true }));

const fake = require('./helpers/fake-electron');
const autostart = require('../linux-runtime/autostart');
const staging = require('../linux-runtime/staging');
const secure = require('../linux-runtime/secure-fs');
const shortcuts = require('../linux-runtime/global-shortcuts');
const polyfills = require('../linux-runtime/polyfills');
const tray = require('../linux-runtime/tray');
const passwordStore = require('../linux-runtime/password-store');
const paths = require('../linux-runtime/paths');
const constants = require('../linux-runtime/constants');

/** Runs `fn` with environment variables set (undefined removes one); restores them afterwards, also for async `fn`. */
function withEnv(vars, fn) {
  const saved = {};
  for (const [k, v] of Object.entries(vars)) {
    saved[k] = process.env[k];
    if (v === undefined) delete process.env[k];
    else process.env[k] = v;
  }
  const restore = () => {
    for (const [k, v] of Object.entries(saved)) {
      if (v === undefined) delete process.env[k];
      else process.env[k] = v;
    }
  };
  let result;
  try {
    result = fn();
  } catch (error) {
    restore();
    throw error;
  }
  if (typeof result?.then === 'function') return result.finally(restore);
  restore();
  return result;
}

test('runtime logs go to $XDG_CONFIG_HOME/Plaud/logs', () => {
  require('../linux-runtime/log').createLogger('test').info('hello');
  const file = require('../linux-runtime/log').logFile();
  assert.ok(file.startsWith(path.join(ROOT, 'config', 'Plaud', 'logs', 'linux-runtime-')));
  assert.match(fs.readFileSync(file, 'utf8'), /\[info\] \[linux:test\] hello/);
});

test('desktop entry Exec quoting follows the Desktop Entry specification', () => {
  assert.equal(autostart.quoteExecArg('/opt/plaud-linux/plaud-linux'), '/opt/plaud-linux/plaud-linux');
  assert.equal(autostart.quoteExecArg('/home/a b/plaud'), '"/home/a b/plaud"');
  assert.equal(autostart.quoteExecArg('/x/100%/p'), '/x/100%%/p');
  assert.equal(autostart.quoteExecArg('/x/$HOME"`'), '"/x/\\\\$HOME\\\\"\\\\`"');
  assert.equal(autostart.quoteExecArg('/x\\y'), '"/x\\\\\\\\y"');
});

test('autostart writes, reads, migrates and removes the XDG autostart entry', () => {
  withEnv({ PLAUD_LINUX_LAUNCHER: '/opt/My Apps/plaud-linux', APPIMAGE: undefined }, () => {
    const entry = autostart.entryPath();
    assert.equal(entry, path.join(ROOT, 'config', 'autostart', `${constants.APP_ID}.desktop`));
    assert.equal(autostart.getAutoLaunch(), false);
    autostart.setAutoLaunch(true);
    const text = fs.readFileSync(entry, 'utf8');
    assert.match(text, /^Exec="\/opt\/My Apps\/plaud-linux" --auto-launch$/m);
    assert.equal(autostart.getAutoLaunch(), true);
    const validate = spawnSync('desktop-file-validate', [entry], { encoding: 'utf8' });
    if (!validate.error) assert.equal(validate.status, 0, validate.stdout + validate.stderr);

    // An entry disabled by the desktop counts as off.
    fs.writeFileSync(entry, `${text}Hidden=true\n`);
    assert.equal(autostart.getAutoLaunch(), false);
    fs.writeFileSync(entry, text);

    // A moved installation rewrites the enabled entry.
    process.env.PLAUD_LINUX_LAUNCHER = '/usr/bin/plaud-linux';
    autostart.migrateAutoLaunch();
    assert.match(fs.readFileSync(entry, 'utf8'), /^Exec=\/usr\/bin\/plaud-linux --auto-launch$/m);

    autostart.setAutoLaunch(false);
    assert.ok(!fs.existsSync(entry));
    autostart.migrateAutoLaunch();
    assert.ok(!fs.existsSync(entry), 'migration never enables autostart');
  });
});

test('unpackaged runs start the app path from autostart', () => {
  withEnv({ PLAUD_LINUX_LAUNCHER: '/dev/electron/plaud-linux-dev' }, () => {
    const savedArgv = process.argv;
    process.defaultApp = true;
    process.argv = ['/dev/electron/electron', '/work/out/app.asar'];
    try {
      assert.deepEqual(autostart.launchCommand(), ['/dev/electron/plaud-linux-dev', '/work/out/app.asar']);
    } finally {
      delete process.defaultApp;
      process.argv = savedArgv;
    }
    assert.deepEqual(autostart.launchCommand(), ['/dev/electron/plaud-linux-dev']);
  });
});

/** Stages `version` as patcher/cli.js stage does: private directory, stage.json with the checksums written last. */
function stage(version, runtimeId, { checksums = true } = {}) {
  const dir = secure.ensurePrivateDir(path.join(paths.versionsDir(), version), paths.dataHome());
  fs.writeFileSync(path.join(dir, 'app.asar'), `asar ${version}`, { mode: 0o644 });
  const files = checksums ? secure.hashTree(dir) : undefined;
  secure.writePrivateFile(path.join(dir, 'stage.json'), JSON.stringify({ version, runtimeId, files }));
  return dir;
}

function resetStaging() {
  fs.rmSync(paths.dataDir(), { recursive: true, force: true });
  fs.rmSync(paths.stateDir(), { recursive: true, force: true });
}

const modeOf = (file) => fs.statSync(file).mode & 0o777;

test('the loader boots the newest healthy staged release of this build', () => {
  resetStaging();
  stage('1.4.2', 'R1');
  stage('1.4.3', 'R2'); // staged by another build
  stage('1.4.0', 'R1'); // older than the packaged release
  fs.mkdirSync(path.join(paths.versionsDir(), '1.5.0')); // incomplete (no stage.json)
  const pick = () => staging.selectForBoot({ packagedVersion: '1.4.1', runtimeId: 'R1' });

  assert.equal(pick().version, '1.4.2');
  assert.equal(staging.markHealthy('1.4.2'), true);
  assert.equal(staging.markHealthy('1.4.2'), false, 'only a pending attempt is cleared');
  assert.equal(pick().version, '1.4.2', 'a healthy release keeps being used');

  // Two starts without reaching a healthy state mark the release bad.
  assert.equal(pick().version, '1.4.2');
  const messages = [];
  assert.equal(staging.selectForBoot({ packagedVersion: '1.4.1', runtimeId: 'R1', log: (m) => messages.push(m) }), null);
  assert.match(messages[0], /did not start successfully 2 times/);
  assert.equal(pick(), null, 'bad releases stay excluded');
  assert.ok(staging.readState('bad-versions.json', {})['1.4.2']);
});

test('private directories and files are created 0700 and 0600 whatever the umask', () => {
  for (const mask of [0o000, 0o002]) {
    const base = path.join(ROOT, `umask-${mask.toString(8)}`);
    const previous = process.umask(mask);
    try {
      const dir = secure.ensurePrivateDir(path.join(base, 'a', 'b'), base);
      for (const d of [base, path.join(base, 'a'), dir]) assert.equal(modeOf(d), 0o700, `${d}, umask ${mask.toString(8)}`);
      secure.writePrivateFile(path.join(dir, 'state.json'), '{}');
      const { fd, file } = secure.createPrivateFile(dir, 'download');
      fs.closeSync(fd);
      for (const f of [path.join(dir, 'state.json'), file]) assert.equal(modeOf(f), 0o600, `${f}, umask ${mask.toString(8)}`);
      assert.deepEqual(fs.readdirSync(dir).filter((n) => n.endsWith('.tmp')), [], 'no temporary file is left');

      // Staged releases and the boot bookkeeping use the same helpers.
      resetStaging();
      const staged = stage('1.4.2', 'R1');
      assert.equal(staging.selectForBoot({ packagedVersion: '1.4.1', runtimeId: 'R1' }).version, '1.4.2');
      for (const d of [paths.dataDir(), paths.versionsDir(), staged, paths.stateDir()]) assert.equal(modeOf(d), 0o700, `${d}, umask ${mask.toString(8)}`);
      for (const f of [path.join(staged, 'stage.json'), path.join(paths.stateDir(), 'boot.json')]) assert.equal(modeOf(f), 0o600, `${f}, umask ${mask.toString(8)}`);
    } finally {
      process.umask(previous);
    }
  }
  resetStaging();
});

test('private directories and files are refused when they are links, shared or not ours', () => {
  const base = path.join(ROOT, 'checks');
  const dir = secure.ensurePrivateDir(path.join(base, 'own', 'sub'), base);
  fs.chmodSync(dir, 0o775);
  assert.throws(() => secure.ensurePrivateDir(dir, base), { code: 'ERR_UNSAFE_PATH', message: /sub is writable by its group \(mode 0775\)$/ });
  fs.chmodSync(dir, 0o703);
  assert.throws(() => secure.ensurePrivateDir(dir, base), { code: 'ERR_UNSAFE_PATH', message: /sub is writable by other users/ });
  fs.chmodSync(dir, 0o700);
  fs.symlinkSync(path.join(base, 'own'), path.join(base, 'link'));
  assert.throws(() => secure.ensurePrivateDir(path.join(base, 'link', 'sub'), base), { code: 'ERR_UNSAFE_PATH', message: /link is a symbolic link$/ });
  if (process.getuid() !== 0) {
    assert.throws(() => secure.ensurePrivateDir('/usr', '/'), { code: 'ERR_UNSAFE_PATH', message: /^\/usr is owned by uid 0, not by uid \d+$/ });
  }

  const file = path.join(dir, 'state.json');
  secure.writePrivateFile(file, '{"a":1}');
  assert.equal(secure.readPrivateFile(file).toString(), '{"a":1}');
  assert.equal(secure.readPrivateFile(path.join(dir, 'missing.json')), null);
  fs.symlinkSync(file, path.join(dir, 'link.json'));
  assert.throws(() => secure.readPrivateFile(path.join(dir, 'link.json')), { code: 'ERR_UNSAFE_PATH', message: /link\.json is a symbolic link$/ });
  fs.chmodSync(file, 0o620);
  assert.throws(() => secure.readPrivateFile(file), { code: 'ERR_UNSAFE_PATH', message: /state\.json is writable by its group/ });
  const fifo = path.join(dir, 'fifo');
  if (spawnSync('mkfifo', [fifo]).status === 0) {
    assert.throws(() => secure.readPrivateFile(fifo), { code: 'ERR_UNSAFE_PATH', message: /fifo is not a file$/ }, 'a FIFO is refused without blocking');
  }
});

test('a staged release starts only from private directories and unmodified', () => {
  const versions = paths.versionsDir();
  const release = path.join(versions, '1.4.2');
  const pick = (versionsDir = versions) => {
    const messages = [];
    const picked = staging.selectForBoot({ packagedVersion: '1.4.1', runtimeId: 'R1', versionsDir, log: (m) => messages.push(m) });
    return { version: picked?.version ?? null, log: messages.join('\n') };
  };
  const cases = [
    ['a modified archive', () => fs.appendFileSync(path.join(release, 'app.asar'), 'x'), /app\.asar does not match its SHA-256 checksum/],
    ['a missing archive', () => fs.rmSync(path.join(release, 'app.asar')), /app\.asar is missing/],
    [
      'an added file',
      () => {
        fs.mkdirSync(path.join(release, 'app.asar.unpacked'));
        fs.writeFileSync(path.join(release, 'app.asar.unpacked', 'evil.node'), 'x');
      },
      /app\.asar\.unpacked\/evil\.node is not part of the staged release/,
    ],
    [
      'an archive behind a symbolic link',
      () => {
        const real = path.join(ROOT, 'elsewhere.asar');
        fs.renameSync(path.join(release, 'app.asar'), real);
        fs.symlinkSync(real, path.join(release, 'app.asar'));
      },
      /app\.asar is a symbolic link/,
    ],
    ['a stage.json writable by other users', () => fs.chmodSync(path.join(release, 'stage.json'), 0o602), /stage\.json is writable by other users/],
    ['a group-writable release directory', () => fs.chmodSync(release, 0o770), /1\.4\.2 is writable by its group/],
    ['a group-writable versions directory', () => fs.chmodSync(versions, 0o775), /versions is writable by its group/],
    ['a data directory writable by other users', () => fs.chmodSync(paths.dataDir(), 0o707), /data\/plaud-linux is writable by other users/],
    [
      'a versions directory that is a symbolic link',
      () => {
        const real = path.join(paths.dataDir(), 'versions-real');
        fs.renameSync(versions, real);
        fs.symlinkSync(real, versions);
      },
      /versions is a symbolic link/,
    ],
    ['a group-writable state directory', () => fs.chmodSync(paths.stateDir(), 0o770), /state\/plaud-linux is writable by its group/],
    ['a release staged without checksums', () => stage('1.4.2', 'R1', { checksums: false }), /stage\.json records no file checksums/],
  ];
  for (const [what, damage, reason] of cases) {
    resetStaging();
    stage('1.4.2', 'R1');
    assert.equal(pick().version, '1.4.2', `${what}: the intact release starts`);
    damage();
    const { version, log } = pick();
    assert.equal(version, null, `${what}: the packaged release starts`);
    assert.match(log, reason, what);
  }

  // A versions directory of another user (here root's /usr/share).
  if (process.getuid() !== 0) {
    const { version, log } = pick('/usr/share');
    assert.equal(version, null);
    assert.match(log, /^staged releases not used: \/usr is owned by uid 0, not by uid \d+$/);
  }
  resetStaging();
});

test('pruning keeps the release in use and the newest usable one', () => {
  resetStaging();
  for (const [v, r] of [['1.4.2', 'R1'], ['1.4.3', 'R1'], ['1.4.4', 'R2'], ['1.4.0', 'R1']]) stage(v, r);
  const old = path.join(paths.versionsDir(), '.tmp-1.4.5-abc');
  fs.mkdirSync(old);
  const sevenHoursAgo = (Date.now() - 7 * 3600 * 1000) / 1000;
  fs.utimesSync(old, sevenHoursAgo, sevenHoursAgo);
  const removed = staging.pruneStaged({ packagedVersion: '1.4.1', runtimeId: 'R1', keepVersion: '1.4.2' });
  assert.deepEqual(removed.sort(), ['1.4.0', '1.4.4']);
  assert.deepEqual(staging.listStaged().map((s) => s.version), ['1.4.3', '1.4.2']);
  assert.ok(!fs.existsSync(old), 'stale staging leftovers are removed');
  assert.equal(staging.findStaged({ version: '1.4.3', runtimeId: 'R1' }).version, '1.4.3');
  fs.appendFileSync(path.join(paths.versionsDir(), '1.4.3', 'app.asar'), 'x');
  assert.equal(staging.findStaged({ version: '1.4.3', runtimeId: 'R1' }), null, 'a modified release counts as not staged');
});

test('refused releases are remembered per build', () => {
  assert.equal(staging.recordRefused('2.0.0', 'R1', 'anchor not found'), true);
  assert.equal(staging.recordRefused('2.0.0', 'R1', 'anchor not found'), false, 'reported once');
  assert.ok(staging.isRefused('2.0.0', 'R1'));
  assert.ok(!staging.isRefused('2.0.0', 'R2'), 'a newer build may support it');
  assert.equal(modeOf(path.join(paths.stateDir(), 'refused-updates.json')), 0o600);
  assert.throws(() => staging.readBuildInfo(path.join(ROOT, 'nowhere')), /build-info\.json/);
});

test('Electron accelerators map to XDG shortcut triggers', () => {
  assert.equal(shortcuts.toPortalTrigger('Alt+Shift+H'), 'ALT+SHIFT+h');
  assert.equal(shortcuts.toPortalTrigger('CommandOrControl+Shift+S'), 'CTRL+SHIFT+s');
  assert.equal(shortcuts.toPortalTrigger('Super+F12'), 'LOGO+F12');
  assert.equal(shortcuts.toPortalTrigger('Ctrl+Space'), 'CTRL+space');
  assert.equal(shortcuts.toPortalTrigger('Alt+/'), 'ALT+slash');
  assert.equal(shortcuts.toPortalTrigger('Ctrl+7'), 'CTRL+7');
  assert.equal(shortcuts.toPortalTrigger('Alt+MediaPlayPause'), null, 'unknown keys are not bound');
  assert.equal(shortcuts.toPortalTrigger(''), null);
});

test('the portal is used in Wayland sessions unless disabled', () => {
  withEnv({ PLAUD_LINUX_SHORTCUTS: undefined, WAYLAND_DISPLAY: 'wayland-1', XDG_SESSION_TYPE: 'wayland' }, () => assert.equal(shortcuts.portalWanted(), true));
  withEnv({ PLAUD_LINUX_SHORTCUTS: undefined, WAYLAND_DISPLAY: undefined, XDG_SESSION_TYPE: 'x11' }, () => assert.equal(shortcuts.portalWanted(), false));
  withEnv({ PLAUD_LINUX_SHORTCUTS: 'electron', WAYLAND_DISPLAY: 'wayland-1' }, () => assert.equal(shortcuts.portalWanted(), false));
  withEnv({ PLAUD_LINUX_SHORTCUTS: 'portal', WAYLAND_DISPLAY: undefined, XDG_SESSION_TYPE: 'x11' }, () => assert.equal(shortcuts.portalWanted(), true));
});

test('without the portal, shortcuts register through Electron only', () => {
  withEnv({ PLAUD_LINUX_SHORTCUTS: 'electron' }, () => {
    shortcuts.install();
    let fired = 0;
    assert.equal(shortcuts.register('highlight', 'Alt+Shift+H', () => fired++), true);
    assert.ok(fake.globalShortcut.isRegistered('Alt+Shift+H'));
    fake.globalShortcut.registered.get('Alt+Shift+H')();
    assert.equal(fired, 1);
    fake.globalShortcut.unregister('Alt+Shift+H');
    assert.ok(!fake.globalShortcut.isRegistered('Alt+Shift+H'));
  });
});

test('missing macOS/Windows media-access APIs are provided', async () => {
  delete fake.systemPreferences.getMediaAccessStatus;
  delete fake.systemPreferences.askForMediaAccess;
  polyfills.installEarly();
  assert.equal(fake.systemPreferences.getMediaAccessStatus('microphone'), 'granted');
  assert.equal(await fake.systemPreferences.askForMediaAccess('microphone'), true);
});

test('the ozone platform is derived from switches and the session', () => {
  const switches = fake.app.commandLine.switches;
  switches.clear();
  withEnv({ WAYLAND_DISPLAY: 'wayland-1', ELECTRON_OZONE_PLATFORM_HINT: undefined }, () => {
    assert.equal(polyfills.ozonePlatform(), 'x11');
    switches.set('ozone-platform-hint', 'auto');
    assert.equal(polyfills.ozonePlatform(), 'wayland');
    switches.set('ozone-platform', 'x11');
    assert.equal(polyfills.ozonePlatform(), 'x11', 'an explicit platform wins');
  });
  switches.clear();
});

test('on Wayland the cursor position is only queried while a window is visible', () => {
  const switches = fake.app.commandLine.switches;
  switches.set('ozone-platform', 'wayland');
  const original = fake.screen.getCursorScreenPoint;
  try {
    withEnv({ HYPRLAND_INSTANCE_SIGNATURE: undefined }, () => {
      fake.screen.cursorCalls = 0;
      polyfills.installAfterReady();
      fake.BrowserWindow.windows = [];
      assert.deepEqual(fake.screen.getCursorScreenPoint(), { x: 960, y: 540 }, 'primary display centre');
      assert.equal(fake.screen.cursorCalls, 0, 'Chromium is not asked without a visible window');
      fake.BrowserWindow.windows = [{ isDestroyed: () => false, isVisible: () => true }];
      fake.screen.cursor = { x: 5, y: 6 };
      assert.deepEqual(fake.screen.getCursorScreenPoint(), { x: 5, y: 6 });
      assert.equal(fake.screen.cursorCalls, 1);
      fake.BrowserWindow.windows = [];
      assert.deepEqual(fake.screen.getCursorScreenPoint(), { x: 5, y: 6 }, 'last known position afterwards');
    });
  } finally {
    fake.screen.getCursorScreenPoint = original;
    switches.clear();
  }
});

const PANEL_URL = 'file:///opt/plaud-linux/resources/app.asar/out-global-online/renderer/window/recording-main/index.html';

function fakeTray() {
  const { EventEmitter } = require('node:events');
  return Object.assign(new EventEmitter(), { menu: null, isDestroyed: () => false, setContextMenu(menu) { this.menu = menu; } });
}

function fakeWindow(url, visible) {
  const events = [];
  return { events, isDestroyed: () => false, isVisible: () => visible, webContents: { getURL: () => url }, emit: (name) => events.push(name) };
}

test('tray icon paths, tray bounds before a click and the tray context menu', () => {
  assert.ok(tray.iconPath('on').endsWith(path.join('resources', 'tray', 'linux', 'Icon-on.png')));
  assert.ok(tray.iconPath('update-available').endsWith('Icon-update.png'));
  assert.ok(tray.iconPath('unknown').endsWith('Icon-off.png'));

  withEnv({ XDG_SESSION_TYPE: 'wayland', WAYLAND_DISPLAY: 'wayland-1', HYPRLAND_INSTANCE_SIGNATURE: undefined }, () => {
    // The official code puts the panel below tray bounds in the upper half of the display, else above.
    assert.deepEqual(tray.getBounds(), { x: 1920 - 24 - 8, y: 32 - 24, width: 24, height: 24 }, 'top panel: in the bar at the top-right corner');
    const saved = fake.screen.displays;
    const display = (workArea) => [{ id: 1, bounds: { x: 0, y: 0, width: 1920, height: 1080 }, workArea, scaleFactor: 1 }];
    fake.screen.displays = display({ x: 0, y: 0, width: 1920, height: 1040 });
    assert.deepEqual(tray.getBounds(), { x: 1888, y: 1040, width: 24, height: 24 }, 'bottom panel: in the bar at the bottom-right corner');
    fake.screen.displays = display({ x: 40, y: 0, width: 1880, height: 1080 });
    assert.deepEqual(tray.getBounds(), { x: 40, y: 1080 - 24, width: 24, height: 24 }, 'left panel: bottom-left corner of the work area');
    fake.screen.displays = display({ x: 0, y: 0, width: 1880, height: 1080 });
    assert.deepEqual(tray.getBounds(), { x: 1880 - 24, y: 1080 - 24, width: 24, height: 24 }, 'right panel: bottom-right corner of the work area');
    fake.screen.displays = display({ x: 0, y: 0, width: 1920, height: 1080 });
    assert.deepEqual(tray.getBounds(), { x: 1888, y: 0, width: 24, height: 24 }, 'no panel: top-right corner');
    fake.screen.displays = saved;

    const icon = fakeTray();
    let language = 'en';
    tray.attachContextMenu(icon, () => [{ id: 'quit', label: language === 'en' ? 'Quit' : 'Beenden', click() {} }]);
    assert.equal(icon.menu.template[0].label, 'Quit');
    language = 'de';
    require('i18next').emit('languageChanged');
    assert.equal(icon.menu.template[0].label, 'Beenden', 'the menu follows the UI language');

    fake.BrowserWindow.windows = [fakeWindow('file:///other/index.html', true)];
    icon.emit('click');
    assert.deepEqual(fake.BrowserWindow.windows[0].events, [], 'other windows are left alone');
    assert.deepEqual(tray.getBounds(), { x: 1888, y: 8, width: 24, height: 24 }, 'the pointer is unknown here: still the corner');
  });
});

test('a tray click hides the open tray panel and anchors the panel at the pointer', () => {
  const icon = fakeTray();
  tray.attachContextMenu(icon, () => []);
  const savedDisplays = fake.screen.displays;
  try {
    withEnv({ XDG_SESSION_TYPE: 'x11', WAYLAND_DISPLAY: undefined, HYPRLAND_INSTANCE_SIGNATURE: undefined }, () => {
      const hidden = fakeWindow(PANEL_URL, false);
      fake.BrowserWindow.windows = [hidden];
      fake.screen.cursor = { x: 1700, y: 10 };
      icon.emit('click');
      assert.deepEqual(hidden.events, [], 'a hidden panel is left to the official handler, which shows it');
      assert.deepEqual(tray.getBounds(), { x: 1688, y: 8, width: 24, height: 24 }, 'X11: under the pointer, in the top panel');
      const open = fakeWindow(PANEL_URL, true);
      fake.BrowserWindow.windows = [open];
      icon.emit('click');
      assert.deepEqual(open.events, ['blur'], 'an open panel gets the blur that hides it, as on Windows');

      fake.BrowserWindow.windows = [];
      fake.screen.displays = [{ id: 1, bounds: { x: 0, y: 0, width: 1920, height: 1080 }, workArea: { x: 0, y: 0, width: 1920, height: 1080 }, scaleFactor: 1 }];
      fake.screen.cursor = { x: 500, y: 700 };
      icon.emit('click');
      assert.deepEqual(tray.getBounds(), { x: 488, y: 688, width: 24, height: 24 }, 'later clicks move the anchor; without panels it is the pointer');
    });
  } finally {
    fake.BrowserWindow.windows = [];
    fake.screen.displays = savedDisplays;
  }
});

// A two-monitor layout: a 2x laptop panel and a 1x wide monitor above it, both
// with a 28 px bar on the left; XWayland lays the outputs out side by side in
// device pixels (xwayland:force_zero_scaling) and Electron scales them by 2.
const HYPR_MONITORS = [
  { id: 0, name: 'eDP-1', x: 0, y: 0, width: 3840, height: 2400, scale: 2, transform: 0, reserved: [28, 0, 0, 0], disabled: false },
  { id: 1, name: 'DP-6', x: -320, y: -1080, width: 2560, height: 1080, scale: 1, transform: 0, reserved: [28, 0, 0, 0], disabled: false },
];
const XWAYLAND_DISPLAYS = [
  { id: 11, bounds: { x: 0, y: 0, width: 1920, height: 1200 }, workArea: { x: 0, y: 0, width: 1920, height: 1200 }, scaleFactor: 2 },
  { id: 12, bounds: { x: 1920, y: 0, width: 1280, height: 540 }, workArea: { x: 1920, y: 0, width: 1280, height: 540 }, scaleFactor: 2 },
];

/**
 * Runs `fn` in a fake Hyprland session: a hyprctl on PATH that answers
 * `-j monitors|cursorpos` from JSON files and accepts `eval`, or `keyword
 * <name>`, when a file accept-eval or accept-keyword-<name> exists. It logs
 * those calls to calls.log.
 */
function withFakeHyprland(fn) {
  const dir = fs.mkdtempSync(path.join(ROOT, 'hyprctl-'));
  fs.writeFileSync(
    path.join(dir, 'hyprctl'),
    [
      '#!/bin/sh',
      `dir='${dir}'`,
      'case "$1" in',
      '  -j) case "$2" in monitors|cursorpos) cat "$dir/$2.json" ;; *) exit 1 ;; esac ;;',
      `  eval) printf '%s\\n' "$*" >> "$dir/calls.log"; if [ -f "$dir/accept-eval" ]; then echo ok; else echo 'error: no Lua'; exit 1; fi ;;`,
      `  keyword) printf '%s\\n' "$*" >> "$dir/calls.log"; if [ -f "$dir/accept-keyword-$2" ]; then echo ok; else echo 'config option <x> does not exist'; fi ;;`,
      '  *) exit 1 ;;',
      'esac',
      '',
    ].join('\n'),
    { mode: 0o755 },
  );
  const write = (name, value) => fs.writeFileSync(path.join(dir, `${name}.json`), JSON.stringify(value));
  const remove = (name) => fs.rmSync(path.join(dir, `${name}.json`));
  const accept = (what) => fs.writeFileSync(path.join(dir, `accept-${what}`), '');
  const calls = () => (fs.existsSync(path.join(dir, 'calls.log')) ? fs.readFileSync(path.join(dir, 'calls.log'), 'utf8').trim().split('\n') : []);
  write('monitors', HYPR_MONITORS);
  const hyprland = require('../linux-runtime/hyprland');
  hyprland.reset();
  const env = { PATH: `${dir}:${process.env.PATH}`, HYPRLAND_INSTANCE_SIGNATURE: 'test', WAYLAND_DISPLAY: 'wayland-1', XDG_SESSION_TYPE: 'wayland', XDG_RUNTIME_DIR: dir };
  return withEnv(env, () => {
    let result;
    try {
      result = fn({ hyprland, dir, write, remove, accept, calls });
    } catch (error) {
      hyprland.reset();
      throw error;
    }
    if (typeof result?.then === 'function') return result.finally(() => hyprland.reset());
    hyprland.reset();
    return result;
  });
}

test('Hyprland monitors are paired with Electron displays and points and bars converted', () => {
  withFakeHyprland(({ hyprland }) => {
    assert.ok(hyprland.inSession());
    const pairs = hyprland.pair(hyprland.monitors(), XWAYLAND_DISPLAYS);
    assert.deepEqual(pairs.map((p) => [p.monitor.name, p.display.id]), [['DP-6', 12], ['eDP-1', 11]]);
    assert.deepEqual(hyprland.toScreenPoint({ x: 88, y: 707 }, pairs), { x: 88, y: 707 }, 'scale 2 on both sides');
    assert.deepEqual(hyprland.toScreenPoint({ x: -220, y: -1030 }, pairs), { x: 1970, y: 25 }, 'scale 1 monitor, Electron scale 2');
    assert.equal(hyprland.toScreenPoint({ x: 5000, y: 5000 }, pairs), null, 'outside every monitor');
    assert.deepEqual(hyprland.workAreaOf(XWAYLAND_DISPLAYS[0], pairs), { x: 28, y: 0, width: 1892, height: 1200 });
    assert.deepEqual(hyprland.workAreaOf(XWAYLAND_DISPLAYS[1], pairs), { x: 1934, y: 0, width: 1266, height: 540 });

    const rotated = [{ ...HYPR_MONITORS[0], transform: 1, reserved: [0, 30, 0, 0] }];
    const portrait = [{ id: 21, bounds: { x: 0, y: 0, width: 1200, height: 1920 }, workArea: { x: 0, y: 0, width: 1200, height: 1920 }, scaleFactor: 2 }];
    const rp = hyprland.pair(rotated, portrait);
    assert.equal(rp.length, 1, 'a monitor rotated by 90 degrees swaps its sides');
    assert.deepEqual(hyprland.workAreaOf(portrait[0], rp), { x: 0, y: 30, width: 1200, height: 1890 });

    const twins = [
      { ...HYPR_MONITORS[1], name: 'B', x: 2560, y: 0, reserved: [0, 0, 0, 40] },
      { ...HYPR_MONITORS[1], name: 'A', x: 0, y: 0, reserved: [0, 40, 0, 0] },
    ];
    const twinDisplays = [
      { id: 32, bounds: { x: 2560, y: 0, width: 2560, height: 1080 }, workArea: { x: 2560, y: 0, width: 2560, height: 1080 }, scaleFactor: 1 },
      { id: 31, bounds: { x: 0, y: 0, width: 2560, height: 1080 }, workArea: { x: 0, y: 0, width: 2560, height: 1080 }, scaleFactor: 1 },
    ];
    assert.deepEqual(hyprland.pair(twins, twinDisplays).map((p) => [p.monitor.name, p.display.id]), [['A', 31], ['B', 32]], 'equal sizes pair in layout order');
    assert.equal(hyprland.withWorkArea(portrait[0], pairs), portrait[0], 'an unpaired display is returned unchanged');
  });
});

test('in Hyprland sessions the screen module reports bars and the real pointer, and the tray panel goes next to the bar', () => {
  const saved = {};
  for (const name of ['getAllDisplays', 'getPrimaryDisplay', 'getDisplayNearestPoint', 'getDisplayMatching', 'getCursorScreenPoint']) saved[name] = fake.screen[name];
  const savedDisplays = fake.screen.displays;
  const savedShowInactive = fake.BrowserWindow.prototype.showInactive;
  const switches = fake.app.commandLine.switches;
  switches.set('ozone-platform', 'x11');
  try {
    withFakeHyprland(({ write, remove }) => {
      fake.screen.displays = XWAYLAND_DISPLAYS;
      polyfills.installAfterReady();
      assert.deepEqual(fake.screen.getAllDisplays().map((d) => d.workArea.x), [28, 1934], 'work areas exclude the bars');
      assert.deepEqual(fake.screen.getPrimaryDisplay().workArea, { x: 28, y: 0, width: 1892, height: 1200 });
      assert.deepEqual(fake.screen.getDisplayNearestPoint({ x: 2000, y: 10 }).workAreaSize, { width: 1266, height: 540 });
      assert.equal(fake.screen.getDisplayMatching({ x: 100, y: 100, width: 10, height: 10 }).id, 11);
      assert.deepEqual(XWAYLAND_DISPLAYS[0].workArea, { x: 0, y: 0, width: 1920, height: 1200 }, "Electron's objects are not modified");

      write('cursorpos', { x: -220, y: -1030 });
      fake.screen.cursorCalls = 0;
      assert.deepEqual(fake.screen.getCursorScreenPoint(), { x: 1970, y: 25 }, 'pointer from the compositor');
      assert.equal(fake.screen.cursorCalls, 0);

      // Click on the tray at the bottom of the left bar of the laptop panel.
      write('cursorpos', { x: 14, y: 1100 });
      const icon = fakeTray();
      tray.attachContextMenu(icon, () => []);
      fake.BrowserWindow.windows = [];
      icon.emit('click');
      const bounds = tray.getBounds();
      assert.deepEqual(bounds, { x: 28, y: 1088, width: 24, height: 24 }, 'at the inner edge of the bar');
      // The official placement (index-C7esC3cC.js:497-513, panel 280x215 on Linux):
      const workArea = fake.screen.getDisplayNearestPoint(bounds).workArea;
      const newX = Math.max(workArea.x, Math.min(bounds.x, workArea.x + workArea.width - 280 - 12));
      let newY = bounds.y > workArea.height / 2 ? bounds.y - 215 : bounds.y + bounds.height;
      newY = Math.max(workArea.y, Math.min(newY, workArea.y + workArea.height - 215 - 8));
      assert.deepEqual({ newX, newY }, { newX: 28, newY: 873 }, 'the panel opens beside the bar, above the tray icon');

      remove('cursorpos');
      fake.screen.cursor = { x: 7, y: 8 };
      assert.deepEqual(fake.screen.getCursorScreenPoint(), { x: 7, y: 8 }, 'falls back to Electron when hyprctl fails');
    });
  } finally {
    Object.assign(fake.screen, saved);
    fake.screen.displays = savedDisplays;
    fake.BrowserWindow.prototype.showInactive = savedShowInactive;
    switches.clear();
  }
});

test('Hyprland gets a window rule in the newest syntax it accepts, again after a configuration reload', async () => {
  await withFakeHyprland(async ({ hyprland, dir, accept, calls }) => {
    assert.equal(hyprland.ensureInactiveRule(), false, 'no syntax accepted');
    assert.deepEqual(
      calls().map((c) => c.split(' ').slice(0, 2).join(' ')),
      ['eval hl.window_rule({', 'keyword windowrule', 'keyword windowrulev2'],
      'tried newest first',
    );
    assert.equal(
      calls()[0],
      'eval hl.window_rule({ name = "io.github.trukhinyuri.PlaudLinux.inactive", match = { class = "^(Plaud|io\\\\.github\\\\.trukhinyuri\\\\.PlaudLinux)$", title = "^Plaud popup$" }, no_initial_focus = true })',
    );
    assert.equal(calls()[2], 'keyword windowrulev2 noinitialfocus, class:^(Plaud|io\\.github\\.trukhinyuri\\.PlaudLinux)$, title:^Plaud popup$');

    hyprland.reset();
    accept('keyword-windowrule');
    assert.equal(hyprland.ensureInactiveRule(), true, 'hyprlang with match: props (0.53, 0.54)');
    assert.equal(calls().at(-1), 'keyword windowrule match:class ^(Plaud|io\\.github\\.trukhinyuri\\.PlaudLinux)$, match:title ^Plaud popup$, no_initial_focus on');

    // With the event socket, the rule is added once per configuration.
    hyprland.reset();
    accept('eval');
    const socketDir = path.join(dir, 'hypr', 'test');
    fs.mkdirSync(socketDir, { recursive: true });
    const server = require('node:net').createServer();
    const connection = new Promise((resolve) => server.once('connection', resolve));
    await new Promise((resolve) => server.listen(path.join(socketDir, '.socket2.sock'), resolve));
    let client = null;
    try {
      const before = calls().length;
      assert.equal(hyprland.ensureInactiveRule(), true, 'Lua (0.55+)');
      client = await connection;
      assert.equal(hyprland.ensureInactiveRule(), true);
      assert.equal(calls().length, before + 1, 'not added again while the configuration stays');
      client.write('activewindow>>foot,ai\nconfigreloaded>>\n');
      await new Promise((resolve) => setTimeout(resolve, 100));
      assert.equal(hyprland.ensureInactiveRule(), true);
      assert.equal(calls().length, before + 2, 'added again after a reload');
    } finally {
      hyprland.reset();
      client?.destroy();
      await new Promise((resolve) => server.close(resolve));
    }
  });
});

test('in Hyprland sessions windows shown inactive map under the marker title and keep their title', (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const savedShowInactive = fake.BrowserWindow.prototype.showInactive;
  try {
    withFakeHyprland(({ hyprland, accept }) => {
      hyprland.installInactiveShow(fake.BrowserWindow);
      const hiddenWithoutRule = new fake.BrowserWindow({ title: 'Plaud', show: false });
      hiddenWithoutRule.showInactive();
      assert.deepEqual(hiddenWithoutRule.mapTitles, ['Plaud'], 'without a rule nothing changes');

      hyprland.reset();
      accept('eval');
      const tip = new fake.BrowserWindow({ title: 'Plaud', show: false });
      tip.showInactive();
      assert.deepEqual(tip.mapTitles, [hyprland.INACTIVE_TITLE], 'mapped under the marker title');
      t.mock.timers.tick(999);
      assert.equal(tip.getTitle(), hyprland.INACTIVE_TITLE);
      t.mock.timers.tick(1);
      assert.equal(tip.getTitle(), 'Plaud', 'its own title afterwards');

      const renamed = new fake.BrowserWindow({ title: 'Plaud', show: false });
      renamed.showInactive();
      renamed.setTitle('Meeting detected');
      t.mock.timers.tick(1000);
      assert.equal(renamed.getTitle(), 'Meeting detected', 'a title the app set meanwhile is kept');

      const visible = new fake.BrowserWindow({ title: 'Plaud', show: true });
      visible.showInactive();
      assert.equal(visible.getTitle(), 'Plaud', 'a visible window is not mapped again');
    });
  } finally {
    fake.BrowserWindow.prototype.showInactive = savedShowInactive;
  }
});

test('the password store follows the desktop and the available keyring service', () => {
  const switches = fake.app.commandLine.switches;
  const noBus = { DBUS_SESSION_BUS_ADDRESS: 'unix:path=/nonexistent/bus', XDG_DATA_DIRS: path.join(ROOT, 'empty') };
  switches.clear();
  withEnv({ ...noBus, XDG_CURRENT_DESKTOP: 'GNOME' }, () => assert.equal(passwordStore.configureBeforeReady(), null));
  assert.ok(!switches.has('password-store'), 'Chromium decides on desktops it knows');

  withEnv({ ...noBus, XDG_CURRENT_DESKTOP: 'Hyprland' }, () => {
    assert.equal(passwordStore.configureBeforeReady(), null, 'no keyring service at all');
    const services = path.join(process.env.XDG_DATA_HOME, 'dbus-1', 'services');
    fs.mkdirSync(services, { recursive: true });
    fs.writeFileSync(path.join(services, 'org.freedesktop.secrets.service'), '[D-BUS Service]\nName=org.freedesktop.secrets\n');
    try {
      assert.equal(passwordStore.configureBeforeReady(), 'gnome-libsecret', 'an activatable Secret Service is selected');
      assert.equal(switches.get('password-store'), 'gnome-libsecret');
      assert.equal(passwordStore.configureBeforeReady(), 'gnome-libsecret', 'an explicit switch is kept');
    } finally {
      fs.rmSync(services, { recursive: true, force: true });
      switches.clear();
    }
  });

  fake.safeStorage.available = true;
  fake.safeStorage.plain = false;
  assert.deepEqual(passwordStore.ensureEncryptionAfterReady(), { backend: 'gnome_libsecret', encrypted: true });
  assert.equal(fake.safeStorage.plain, false);
  fake.safeStorage.available = false;
  let notified = 0;
  assert.deepEqual(passwordStore.ensureEncryptionAfterReady({ notify: () => notified++ }), { backend: 'basic_text', encrypted: false });
  assert.equal(fake.safeStorage.plain, true, 'plain-text mode keeps sign-in working');
  assert.equal(notified, 1);
});
