'use strict';

// Launcher, ffmpeg wrapper, desktop metadata and the per-user installer,
// executed for real against temporary directories.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { templateVars, renderPackagingFile } = require('../scripts/build');
const { rpmFileList, versions } = require('../scripts/package');
const { skippedCount } = require('../scripts/build-native');
const constants = require('../linux-runtime/constants');

const REPO = path.resolve(__dirname, '..');

function tmpdir(t, prefix = 'plaud-pkg-') {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function writeExec(file, content) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, content, { mode: 0o755 });
}

/** An app directory whose "Electron" prints its arguments and launcher variable. */
function fakeApp(dir) {
  writeExec(path.join(dir, constants.COMMAND), renderPackagingFile('launcher.sh', templateVars()));
  writeExec(path.join(dir, constants.EXECUTABLE), '#!/bin/sh\necho "LAUNCHER=$PLAUD_LINUX_LAUNCHER"\nfor a in "$@"; do echo "ARG=$a"; done\n');
  fs.writeFileSync(path.join(dir, 'chrome-sandbox'), '');
  return path.join(dir, constants.COMMAND);
}

function launch(launcher, args, env) {
  const res = spawnSync(launcher, args, { encoding: 'utf8', env: { PATH: process.env.PATH, HOME: process.env.HOME, ...env } });
  const argv = res.stdout.split('\n').filter((l) => l.startsWith('ARG=')).map((l) => l.slice(4));
  const launcherVar = (res.stdout.match(/^LAUNCHER=(.*)$/m) ?? [])[1];
  return { ...res, argv, launcherVar };
}

test('the launcher picks XWayland in Wayland sessions and keeps user arguments last', (t) => {
  const dir = tmpdir(t);
  const launcher = fakeApp(dir);
  const config = path.join(dir, 'config');
  fs.mkdirSync(config);
  fs.writeFileSync(path.join(config, 'plaud-linux-flags.conf'), '# comment\n\n--force-device-scale-factor=1.25\n--enable-logging');
  const r = launch(launcher, ['plaud://login?auth_code=x y', '--auto-launch'], { WAYLAND_DISPLAY: 'wayland-1', DISPLAY: ':0', XDG_CONFIG_HOME: config });
  assert.equal(r.status, 0, r.stderr);
  assert.equal(r.argv[0], '--ozone-platform=x11');
  assert.deepEqual(r.argv.slice(-4), ['--force-device-scale-factor=1.25', '--enable-logging', 'plaud://login?auth_code=x y', '--auto-launch']);
  assert.equal(r.launcherVar, launcher, 'autostart and relaunch use the launcher');
});

test('the launcher honours PLAUD_LINUX_OZONE and rejects unknown values', (t) => {
  const launcher = fakeApp(tmpdir(t));
  const wl = launch(launcher, [], { WAYLAND_DISPLAY: 'wayland-1', DISPLAY: ':0', PLAUD_LINUX_OZONE: 'wayland' });
  assert.deepEqual(wl.argv.slice(0, 3), ['--ozone-platform=wayland', '--enable-features=WaylandWindowDecorations', '--enable-wayland-ime']);
  const waylandOnly = launch(launcher, [], { WAYLAND_DISPLAY: 'wayland-1' });
  assert.equal(waylandOnly.argv[0], '--ozone-platform=wayland', 'no XWayland available');
  const x11 = launch(launcher, [], { DISPLAY: ':0' });
  assert.equal(x11.argv[0], '--ozone-platform=x11');
  const bad = launch(launcher, [], { PLAUD_LINUX_OZONE: 'mir' });
  assert.equal(bad.status, 2);
  assert.match(bad.stderr, /must be auto, x11 or wayland/);
  const preset = launch(launcher, [], { DISPLAY: ':0', PLAUD_LINUX_LAUNCHER: '/x/Plaud.AppImage' });
  assert.equal(preset.launcherVar, '/x/Plaud.AppImage', 'AppRun sets the AppImage path');
});

test('the launcher falls back to --no-sandbox only without user namespaces and setuid helper', (t) => {
  const dir = tmpdir(t);
  const launcher = fakeApp(dir);
  const ok = launch(launcher, [], { DISPLAY: ':0' });
  const userns = spawnSync('unshare', ['--user', '--map-root-user', '--pid', '--net', '--fork', 'true']).status === 0;
  assert.equal(ok.argv.includes('--no-sandbox'), !userns);
  const bin = path.join(dir, 'fakebin');
  writeExec(path.join(bin, 'unshare'), '#!/bin/sh\nexit 1\n');
  const restricted = launch(launcher, [], { DISPLAY: ':0', PATH: `${bin}:${process.env.PATH}` });
  assert.ok(restricted.argv.includes('--no-sandbox'));
  assert.match(restricted.stderr, /no usable Chromium sandbox/);
});

test('the ffmpeg wrapper runs PLAUD_FFMPEG or the next ffmpeg on PATH', (t) => {
  const dir = tmpdir(t);
  const resources = path.join(dir, 'resources');
  writeExec(path.join(resources, 'ffmpeg'), fs.readFileSync(path.join(REPO, 'packaging', 'ffmpeg.sh')));
  const system = path.join(dir, 'system');
  writeExec(path.join(system, 'ffmpeg'), '#!/bin/sh\necho "system ffmpeg $*"\n');
  writeExec(path.join(dir, 'custom'), '#!/bin/sh\necho "custom ffmpeg $*"\n');
  const wrapper = path.join(resources, 'ffmpeg');
  const run = (env) => spawnSync(wrapper, ['-i', 'x'], { encoding: 'utf8', env });
  assert.equal(run({ PATH: `${resources}:${system}:/usr/bin:/bin` }).stdout.trim(), 'system ffmpeg -i x', 'skips itself');
  assert.equal(run({ PATH: `${system}:/usr/bin:/bin`, PLAUD_FFMPEG: path.join(dir, 'custom') }).stdout.trim(), 'custom ffmpeg -i x');
  const none = run({ PATH: `${resources}:${path.join(dir, 'empty')}` });
  assert.equal(none.status, 127);
  assert.match(none.stderr, /ffmpeg not found/);
});

test('desktop entry and AppStream metadata validate', (t) => {
  const dir = tmpdir(t);
  const vars = templateVars({ EXEC: constants.COMMAND });
  const desktop = path.join(dir, `${constants.APP_ID}.desktop`);
  fs.writeFileSync(desktop, renderPackagingFile('app.desktop.in', vars));
  const text = fs.readFileSync(desktop, 'utf8');
  assert.match(text, /^Exec=plaud-linux %u$/m);
  assert.match(text, /^MimeType=x-scheme-handler\/plaud;$/m);
  assert.match(text, /^StartupWMClass=Plaud$/m);
  assert.doesNotMatch(text, /@[A-Z_]+@/, 'every placeholder is filled');
  const dfv = spawnSync('desktop-file-validate', [desktop], { encoding: 'utf8' });
  if (dfv.error) t.diagnostic('desktop-file-validate not installed');
  else assert.equal(dfv.status, 0, dfv.stdout + dfv.stderr);

  const metainfo = path.join(dir, `${constants.APP_ID}.metainfo.xml`);
  fs.writeFileSync(metainfo, renderPackagingFile('metainfo.xml.in', vars));
  assert.doesNotMatch(fs.readFileSync(metainfo, 'utf8'), /@[A-Z_]+@/);
  const asv = spawnSync('appstreamcli', ['validate', '--no-net', metainfo], { encoding: 'utf8' });
  if (asv.error) t.diagnostic('appstreamcli not installed');
  else assert.equal(asv.status, 0, asv.stdout + asv.stderr);
});

test('package versions carry both the build and the Plaud version', () => {
  assert.deepEqual(versions({ version: '0.1.0', upstreamVersion: '1.4.1' }), {
    deb: '0.1.0+plaud1.4.1-1',
    rpm: { version: '0.1.0+plaud1.4.1', release: '1' },
    pacman: '0.1.0_plaud1.4.1',
    file: '0.1.0+plaud1.4.1',
  });
});

test('native test runs report their skipped tests (CI fails on any)', (t) => {
  const spec = 'ℹ tests 12\nℹ suites 1\nℹ pass 10\nℹ fail 0\nℹ cancelled 0\nℹ skipped 2\nℹ todo 0\n';
  assert.equal(skippedCount(spec), 2);
  assert.equal(skippedCount('\x1b[34mℹ skipped 0\x1b[39m\n'), 0, 'colours are ignored');
  assert.equal(skippedCount('# tests 3\n# pass 2\n# skipped 1\n# todo 0\n'), 1, 'TAP summaries count too');
  assert.equal(skippedCount('# tests 3\n# pass 2\n# skip 1\n'), 1, 'older TAP summaries too');
  assert.equal(skippedCount('✔ a test (1ms)\n'), null, 'output without a summary is recognised');

  // Real node:test runs (both reporters) with one skipped test. NODE_TEST_CONTEXT
  // marks a process started by the test runner; a nested run must not inherit it.
  const file = path.join(tmpdir(t), 'skip.test.js');
  fs.writeFileSync(file, "require('node:test')('needs pactl', { skip: 'pactl not installed' }, () => {});\n");
  const env = { ...process.env };
  delete env.NODE_TEST_CONTEXT;
  for (const reporter of ['spec', 'tap']) {
    const res = spawnSync(process.execPath, ['--test', `--test-reporter=${reporter}`, file], { encoding: 'utf8', env });
    assert.equal(skippedCount(res.stdout), 1, `${reporter}: ${res.stdout}${res.stderr}`);
  }
});

test('the RPM file list marks chrome-sandbox setuid root', (t) => {
  const root = tmpdir(t);
  for (const rel of [`opt/${constants.COMMAND}/chrome-sandbox`, `opt/${constants.COMMAND}/resources/app.asar`, `usr/share/icons/hicolor/64x64/apps/${constants.APP_ID}.png`]) {
    fs.mkdirSync(path.dirname(path.join(root, rel)), { recursive: true });
    fs.writeFileSync(path.join(root, rel), '');
  }
  const list = rpmFileList(root).split('\n');
  assert.ok(list.includes(`%attr(4755, root, root) "/opt/${constants.COMMAND}/chrome-sandbox"`));
  assert.ok(list.includes(`%dir "/opt/${constants.COMMAND}/resources"`));
  assert.ok(list.includes(`"/opt/${constants.COMMAND}/resources/app.asar"`));
  assert.ok(list.includes(`"/usr/share/icons/hicolor/64x64/apps/${constants.APP_ID}.png"`));
  assert.ok(!list.some((l) => l.includes('%dir "/usr')), 'shared system directories are not owned');
});

/** A minimal `npm run build` output for scripts/install under `dir`. */
function fakeBuild(dir) {
  const build = path.join(dir, 'build');
  writeExec(path.join(build, 'app', constants.COMMAND), '#!/bin/sh\necho launched\n');
  writeExec(path.join(build, 'app', constants.EXECUTABLE), '#!/bin/sh\n');
  fs.mkdirSync(path.join(build, 'share', 'applications'), { recursive: true });
  fs.writeFileSync(path.join(build, 'share', 'applications', `${constants.APP_ID}.desktop`), renderPackagingFile('app.desktop.in', templateVars({ EXEC: constants.COMMAND })));
  fs.mkdirSync(path.join(build, 'share', 'icons', 'hicolor', '64x64', 'apps'), { recursive: true });
  fs.writeFileSync(path.join(build, 'share', 'icons', 'hicolor', '64x64', 'apps', `${constants.APP_ID}.png`), 'png');
  fs.mkdirSync(path.join(build, 'share', 'metainfo'), { recursive: true });
  fs.writeFileSync(path.join(build, 'share', 'metainfo', `${constants.APP_ID}.metainfo.xml`), '<component/>');
  return build;
}

/** The environment of a user whose home is `home` and data directory `dataHome`. */
function userEnv(home, dataHome) {
  return {
    PATH: process.env.PATH,
    HOME: home,
    XDG_DATA_HOME: dataHome,
    XDG_CONFIG_HOME: path.join(home, '.config'),
    XDG_STATE_HOME: path.join(home, '.local', 'state'),
    XDG_CACHE_HOME: path.join(home, '.cache'),
  };
}

const hasXdgMime = () => spawnSync('xdg-mime', ['--version']).status === 0;

test('per-user install, upgrade and uninstall', (t) => {
  // Spaces in every path: the desktop entry needs Exec quoting.
  const dir = tmpdir(t, 'plaud install ');
  const build = fakeBuild(dir);
  const home = path.join(dir, 'home');
  const env = userEnv(home, path.join(home, 'my data'));
  const run = (script, args = []) => spawnSync(path.join(REPO, 'scripts', script), args, { encoding: 'utf8', env });
  // A data directory left group-writable by an older install with umask 0002.
  const base = path.join(env.XDG_DATA_HOME, constants.DATA_DIR_NAME);
  fs.mkdirSync(base, { recursive: true });
  fs.chmodSync(base, 0o775);
  const first = run('install', ['--from', build]);
  assert.equal(first.status, 0, first.stderr);
  assert.equal(fs.statSync(base).mode & 0o777, 0o700, 'the data directory holding staged releases is private');
  const appDir = path.join(base, 'app');
  assert.ok(fs.existsSync(path.join(appDir, constants.COMMAND)));
  assert.equal(fs.readlinkSync(path.join(home, '.local', 'bin', constants.COMMAND)), path.join(appDir, constants.COMMAND));
  const desktop = fs.readFileSync(path.join(env.XDG_DATA_HOME, 'applications', `${constants.APP_ID}.desktop`), 'utf8');
  assert.ok(desktop.includes(`Exec="${path.join(appDir, constants.COMMAND)}" %u`), desktop);
  assert.ok(fs.existsSync(path.join(env.XDG_DATA_HOME, 'icons', 'hicolor', '64x64', 'apps', `${constants.APP_ID}.png`)));
  // `xdg-mime query` of xdg-utils 1.1.3 (Ubuntu 22.04 and 24.04) finds no
  // handler whose Exec is quoted or whose data directory has a space (it
  // splits the paths at blanks), so the registration is checked in
  // mimeapps.list here, and the query round trip by the plain-path test below.
  if (hasXdgMime()) {
    const mimeapps = fs.readFileSync(path.join(env.XDG_CONFIG_HOME, 'mimeapps.list'), 'utf8').split('\n');
    const section = mimeapps.indexOf('[Default Applications]');
    assert.ok(section >= 0, mimeapps.join('\n'));
    assert.ok(mimeapps.slice(section + 1).includes(`x-scheme-handler/plaud=${constants.APP_ID}.desktop`), mimeapps.join('\n'));
  }

  fs.writeFileSync(path.join(build, 'app', 'new-file'), 'v2');
  const second = run('install', ['--from', build]);
  assert.equal(second.status, 0, second.stderr);
  assert.ok(fs.existsSync(path.join(appDir, 'new-file')), 'upgrade replaces the app directory');
  assert.deepEqual(fs.readdirSync(path.dirname(appDir)).sort(), ['app'], 'no temporary directories are left');

  // Data written by the app, and the autostart entry it creates.
  fs.mkdirSync(path.join(env.XDG_CONFIG_HOME, 'Plaud'), { recursive: true });
  fs.mkdirSync(path.join(env.XDG_CONFIG_HOME, 'autostart'), { recursive: true });
  fs.writeFileSync(path.join(env.XDG_CONFIG_HOME, 'autostart', `${constants.APP_ID}.desktop`), '[Desktop Entry]\n');
  fs.mkdirSync(path.join(env.XDG_DATA_HOME, constants.DATA_DIR_NAME, 'versions', '1.4.2'), { recursive: true });

  const removed = run('uninstall');
  assert.equal(removed.status, 0, removed.stderr);
  assert.ok(!fs.existsSync(appDir));
  assert.ok(!fs.existsSync(path.join(home, '.local', 'bin', constants.COMMAND)));
  assert.ok(!fs.existsSync(path.join(env.XDG_DATA_HOME, 'applications', `${constants.APP_ID}.desktop`)));
  assert.ok(!fs.existsSync(path.join(env.XDG_CONFIG_HOME, 'autostart', `${constants.APP_ID}.desktop`)));
  assert.ok(!fs.existsSync(path.join(env.XDG_DATA_HOME, constants.DATA_DIR_NAME, 'versions')), 'staged releases are part of the app');
  assert.ok(fs.existsSync(path.join(env.XDG_CONFIG_HOME, 'Plaud')), 'user data is kept without --purge');
  assert.equal(run('uninstall', ['--purge']).status, 0);
  assert.ok(!fs.existsSync(path.join(env.XDG_CONFIG_HOME, 'Plaud')));
});

test('install into a plain path: unquoted Exec, plaud:// resolves through xdg-mime', (t) => {
  const dir = tmpdir(t, 'plaud-install-plain-');
  const build = fakeBuild(dir);
  const home = path.join(dir, 'home');
  const env = userEnv(home, path.join(home, '.local', 'share'));
  const res = spawnSync(path.join(REPO, 'scripts', 'install'), ['--from', build], { encoding: 'utf8', env });
  assert.equal(res.status, 0, res.stderr);
  const exe = path.join(env.XDG_DATA_HOME, constants.DATA_DIR_NAME, 'app', constants.COMMAND);
  const desktop = fs.readFileSync(path.join(env.XDG_DATA_HOME, 'applications', `${constants.APP_ID}.desktop`), 'utf8');
  assert.ok(desktop.split('\n').includes(`Exec=${exe} %u`), desktop);
  if (hasXdgMime()) {
    const handler = spawnSync('xdg-mime', ['query', 'default', 'x-scheme-handler/plaud'], { encoding: 'utf8', env });
    assert.equal(handler.stdout.trim(), `${constants.APP_ID}.desktop`, handler.stderr);
  }
});
