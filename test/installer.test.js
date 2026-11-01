'use strict';

// Installer packages: what they may contain, the rendered scripts, and the
// download checks of plaud-linux-setup, executed against temporary
// directories without network access.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const installer = require('../scripts/package-installer');
const { resolveRelease } = require('../scripts/fetch-official');
const constants = require('../linux-runtime/constants');

const REPO = path.resolve(__dirname, '..');
const LIB = installer.LIB_DIR.slice(1);
const addonsBuilt = ['recorder/build/Release/recorder_linux.node', 'audio-monitor/build/Release/audio_monitor_linux.node'].every((f) =>
  fs.existsSync(path.join(REPO, 'native', f)),
);
const skipStage = addonsBuilt ? false : 'native addons not built (npm run build:native)';

function tmpdir(t, prefix = 'plaud-installer-') {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function files(root) {
  const out = [];
  const walk = (rel) => {
    for (const entry of fs.readdirSync(path.join(root, rel), { withFileTypes: true })) {
      const r = rel ? `${rel}/${entry.name}` : entry.name;
      if (entry.isDirectory()) walk(r);
      else out.push(r);
    }
  };
  walk('');
  return out;
}

test('the installer tree holds only this project\'s files and renders every template', { skip: skipStage }, (t) => {
  const root = path.join(tmpdir(t), 'root');
  installer.stageRoot(root);
  const list = files(root);
  installer.checkAllowed(list, 'staged tree');
  for (const required of ['usr/bin/plaud-linux', 'usr/bin/plaud-linux-setup', `${LIB}/scripts/setup.js`, `${LIB}/official.json`]) {
    assert.ok(list.includes(required), required);
  }
  for (const f of list) {
    assert.doesNotMatch(f, /\.(asar|icns|ico|png|zip|dmg|exe)$/, f);
  }
  for (const script of ['usr/bin/plaud-linux', 'usr/bin/plaud-linux-setup']) {
    const text = fs.readFileSync(path.join(root, script), 'utf8');
    assert.doesNotMatch(text, /@[A-Z0-9_]+@/, `${script} has an unrendered placeholder`);
    assert.equal(spawnSync('sh', ['-n', path.join(root, script)]).status, 0, `${script} parses`);
  }
  const setup = fs.readFileSync(path.join(root, 'usr/bin/plaud-linux-setup'), 'utf8');
  const pinned = require('../scripts/electron-checksums.json')[constants.ELECTRON_VERSION];
  assert.ok(setup.includes(`ELECTRON_SHA256_arm64=${pinned[`electron-v${constants.ELECTRON_VERSION}-linux-arm64.zip`]}`));
  assert.ok(setup.includes(`ELECTRON_SHA256_x64=${pinned[`electron-v${constants.ELECTRON_VERSION}-linux-x64.zip`]}`));
  const desktop = fs.readFileSync(path.join(root, `usr/share/applications/${constants.APP_ID}.desktop`), 'utf8');
  assert.ok(desktop.split('\n').includes(`Exec=${constants.COMMAND} %u`));
});

test('the allowlist refuses Plaud files and foreign binaries', () => {
  const ok = ['usr/bin/plaud-linux', 'usr/bin/plaud-linux-setup', `${LIB}/scripts/setup.js`];
  installer.checkAllowed(ok, 'minimal');
  for (const bad of [
    `${LIB}/resources/app.asar`,
    'opt/plaud-linux/resources/app.asar',
    `${LIB}/native/recorder/build/Release/recorder_mac.node`,
    `${LIB}/icon.icns`,
    `${LIB}/patcher/../app.asar`,
    `${LIB}/Plaud-1.4.1-arm64-mac.zip`,
    'usr/share/icons/hicolor/256x256/apps/io.github.trukhinyuri.PlaudLinux.png',
  ]) {
    assert.throws(() => installer.checkAllowed([...ok, bad], 'tampered'), /must not ship/, bad);
  }
  assert.throws(() => installer.checkAllowed(['usr/bin/plaud-linux'], 'incomplete'), /lacks/);
});

/** Renders plaud-linux-setup against a staged library directory. */
function stagedSetup(t) {
  const dir = tmpdir(t);
  const root = path.join(dir, 'root');
  installer.stageRoot(root);
  const setup = path.join(root, 'usr/bin/plaud-linux-setup');
  const text = fs.readFileSync(setup, 'utf8').replace(/^LIBDIR=.*$/m, `LIBDIR=${path.join(root, LIB)}`);
  fs.writeFileSync(setup, text, { mode: 0o755 });
  const home = path.join(dir, 'home');
  fs.mkdirSync(home);
  return { dir, root, setup, home };
}

test('plaud-linux-setup refuses an Electron download that does not match the pinned SHA-256', { skip: skipStage }, (t) => {
  const { dir, setup, home } = stagedSetup(t);
  // A curl that "downloads" wrong content; no network is used.
  const bin = path.join(dir, 'bin');
  fs.mkdirSync(bin);
  fs.writeFileSync(path.join(bin, 'curl'), '#!/bin/sh\nwhile [ $# -gt 1 ]; do [ "$1" = -o ] && out=$2; shift; done\necho tampered > "$out"\n', { mode: 0o755 });
  const env = { PATH: `${bin}:/usr/bin:/bin`, HOME: home };
  const res = spawnSync('sh', [setup, '--user'], { encoding: 'utf8', env });
  assert.equal(res.status, 1, res.stderr);
  assert.match(res.stderr, /does not match the pinned SHA-256/);
  const work = path.join(home, '.cache', constants.COMMAND, 'setup');
  assert.deepEqual(fs.readdirSync(work).filter((f) => f.endsWith('.zip')), [], 'the mismatching download is deleted');
  assert.equal((fs.statSync(work).mode & 0o777).toString(8), '700');
});

test('plaud-linux-setup reports status, refuses --system without root and removes a per-user copy', { skip: skipStage }, (t) => {
  const { setup, home } = stagedSetup(t);
  const env = { PATH: '/usr/bin:/bin', HOME: home };
  let res = spawnSync('sh', [setup, '--status'], { encoding: 'utf8', env });
  assert.equal(res.status, 0);
  assert.match(res.stdout, /user install: not built/);
  if (process.getuid() !== 0) {
    res = spawnSync('sh', [setup, '--system'], { encoding: 'utf8', env });
    assert.equal(res.status, 1);
    assert.match(res.stderr, /needs root/);
  }
  const info = path.join(home, '.local/share/plaud-linux/app/resources/plaud-linux/build-info.json');
  fs.mkdirSync(path.dirname(info), { recursive: true });
  fs.writeFileSync(info, JSON.stringify({ name: 'x', version: '9.9.9', upstreamVersion: '1.4.1', electron: '36.7.4', arch: 'x64', runtimeId: 'r' }, null, 2));
  res = spawnSync('sh', [setup, '--status'], { encoding: 'utf8', env });
  assert.match(res.stdout, /upstreamVersion: 1\.4\.1/);
  res = spawnSync('sh', [setup, '--remove'], { encoding: 'utf8', env });
  assert.equal(res.status, 0, res.stderr);
  assert.ok(!fs.existsSync(path.join(home, '.local/share/plaud-linux/app')));
});

test('the setup tool validates its arguments before doing any work', () => {
  const res = spawnSync(process.execPath, [path.join(REPO, 'scripts/setup.js'), 'install', '--mode', 'everyone', '--work-dir', os.tmpdir()], { encoding: 'utf8' });
  assert.equal(res.status, 1);
  assert.match(res.stderr, /--mode must be user or system/);
});

test('the launcher starts the per-user build when it matches the installer version', { skip: skipStage }, (t) => {
  const { root, home } = stagedSetup(t);
  const launcher = path.join(root, 'usr/bin/plaud-linux');
  const app = path.join(home, '.local/share/plaud-linux/app');
  fs.mkdirSync(path.join(app, 'resources/plaud-linux'), { recursive: true });
  fs.writeFileSync(path.join(app, constants.COMMAND), '#!/bin/sh\necho "started $*"\n', { mode: 0o755 });
  const version = require('../package.json').version;
  fs.writeFileSync(path.join(app, 'resources/plaud-linux/build-info.json'), `${JSON.stringify({ name: 'x', version }, null, 2)}\n`);
  if (fs.existsSync('/opt/plaud-linux/plaud-linux')) return t.skip('a system-wide build exists on this machine');
  const res = spawnSync('sh', [launcher, 'plaud://login?x=1'], { encoding: 'utf8', env: { PATH: '/usr/bin:/bin', HOME: home } });
  assert.equal(res.status, 0, res.stderr);
  assert.equal(res.stdout.trim(), 'started plaud://login?x=1');
});

test('prerelease channels resolve to the directories the official app uses', async () => {
  await assert.rejects(resolveRelease({ channel: 'nightly' }), /unknown channel "nightly"/);
  const pinned = await resolveRelease({});
  assert.equal(pinned.url, `https://client-download.plaud.ai/desktop/v2/${pinned.file}`);
});
