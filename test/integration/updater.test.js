'use strict';

// End-to-end update flow in a real (headless) Electron main process:
// LinuxAutoUpdater fetches a local electron-builder feed, downloads the
// release zip, verifies it, re-patches it with the bundled patcher in an
// ELECTRON_RUN_AS_NODE child and stages it for the loader. Releases built
// from the official Plaud archive exercise the real patcher:
//   1.4.2  official code, new version number      -> staged (umask 0000: still
//                                                    private), selected for boot
//   1.4.3  an anchor removed                      -> refused once, remembered
//   1.4.4  feed checksum does not match the zip   -> error, nothing staged
//   1.4.5  cache directory is a symbolic link     -> error, nothing downloaded
// and finally a staged archive modified after staging is not selected.

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { spawn, execFileSync } = require('node:child_process');
const { AsarArchive } = require('../../patcher/lib/asar');
const patcher = require('../../patcher');
const constants = require('../../linux-runtime/constants');
const { REPO_ROOT, pinned, officialResources, SKIP_REASON } = require('../helpers/official');

const resources = officialResources();
let skip = resources ? false : SKIP_REASON;
try {
  execFileSync('python3', ['-c', 'import zipfile']);
} catch {
  skip = skip || 'python3 is required to build test release archives';
}

const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'plaud-updater-'));
test.after(() => fs.rmSync(ROOT, { recursive: true, force: true }));

const ENV = {
  XDG_CONFIG_HOME: path.join(ROOT, 'config'),
  XDG_DATA_HOME: path.join(ROOT, 'data'),
  XDG_STATE_HOME: path.join(ROOT, 'state'),
  XDG_CACHE_HOME: path.join(ROOT, 'cache'),
};

function copyCode(src, dst) {
  fs.mkdirSync(dst, { recursive: true });
  for (const entry of fs.readdirSync(src, { withFileTypes: true })) {
    if (entry.isDirectory()) copyCode(path.join(src, entry.name), path.join(dst, entry.name));
    else if (/\.(js|json)$/.test(entry.name)) fs.copyFileSync(path.join(src, entry.name), path.join(dst, entry.name));
  }
}

/** Stock Electron with the resources/plaud-linux layout of a real build. */
async function prepareElectron() {
  const { downloadElectron, extractElectron } = require('../../scripts/lib/electron');
  const dir = path.join(ROOT, 'electron');
  const zip = await downloadElectron({ version: constants.ELECTRON_VERSION, arch: process.arch });
  await extractElectron(zip, dir);
  const meta = path.join(dir, 'resources', constants.RESOURCES_SUBDIR);
  copyCode(path.join(REPO_ROOT, 'patcher'), path.join(meta, 'patcher'));
  copyCode(path.join(REPO_ROOT, 'linux-runtime'), path.join(meta, 'linux-runtime'));

  // Baseline and addon API of the release this "build" was verified against.
  const result = patcher.patchResources({
    resourcesDir: resources,
    outAsar: path.join(ROOT, 'baseline.asar'),
    runtimeDir: path.join(REPO_ROOT, 'linux-runtime'),
    runtimeId: 'itest',
    computeBaseline: true,
  });
  const { used } = result.surface;
  const write = (name, value) => fs.writeFileSync(path.join(meta, name), JSON.stringify(value));
  write('build-info.json', { name: constants.PACKAGE_NAME, version: '0.0.0', upstreamVersion: pinned.version, runtimeId: 'itest' });
  write('api-baseline.json', { nodeFiles: result.surface.nodeFiles, candidates: result.surface.candidates, used });
  write('native-api.json', {
    classes: {
      Recorder: { statics: used.recorderStatics, prototype: used.recorderMethods },
      AudioMonitor: { statics: [], prototype: used.monitorMethods },
    },
  });

  const app = path.join(ROOT, 'app');
  fs.mkdirSync(app);
  fs.writeFileSync(path.join(app, 'package.json'), JSON.stringify({ name: 'plaud-updater-itest', version: pinned.version, main: 'main.js' }));
  fs.writeFileSync(
    path.join(app, 'main.js'),
    `
const { app } = require('electron');
const path = require('node:path');
const { autoUpdater } = require(path.join(process.resourcesPath, 'plaud-linux', 'linux-runtime', 'updater.js'));
const events = [];
for (const name of ['checking-for-update', 'update-available', 'update-not-available', 'download-progress', 'update-downloaded', 'error']) {
  autoUpdater.on(name, (info) => events.push({ name, version: info && info.version, file: info && info.downloadedFile, code: info && info.code }));
}
autoUpdater.forceDevUpdateConfig = true;
autoUpdater.setFeedURL({ url: process.env.FEED_URL });
const failsafe = setTimeout(() => { process.stdout.write('RESULT {"timeout":true}\\n'); app.exit(3); }, 240000);
app.whenReady().then(async () => {
  const runs = [];
  for (let i = 0; i < 2; i++) {
    try {
      const r = await autoUpdater.checkForUpdates();
      runs.push({ available: r.isUpdateAvailable, version: r.updateInfo.version });
    } catch (error) {
      runs.push({ error: error.code || error.message });
    }
  }
  process.stdout.write('RESULT ' + JSON.stringify({ events, runs }) + '\\n');
  clearTimeout(failsafe);
  app.quit();
});
`,
  );
  return { electron: path.join(dir, 'electron'), app };
}

/** Builds a "Plaud.app" zip for `version` from the official resources. */
function buildRelease(version, { breakAnchor = false } = {}) {
  const dir = path.join(ROOT, 'releases', version);
  const res = path.join(dir, 'Plaud.app', 'Contents', 'Resources');
  fs.mkdirSync(res, { recursive: true });
  const archive = AsarArchive.open(path.join(resources, 'app.asar'));
  try {
    const pkg = JSON.parse(archive.readFile('package.json').toString('utf8'));
    archive.writeFile('package.json', Buffer.from(JSON.stringify({ ...pkg, version })));
    if (breakAnchor) {
      const file = archive.listFiles().find((f) => /out-global-online\/main\/nativeRecorder-.*\.js$/.test(f));
      archive.writeFile(file, Buffer.from(archive.readFile(file).toString('utf8').replace('"recorder_mac.node" : "recorder.node"', '"recorder_mac.node" : "recorder_win.node"')));
    }
    archive.write(path.join(res, 'app.asar'));
  } finally {
    archive.close();
  }
  for (const f of fs.readdirSync(resources)) {
    if (f.endsWith('.node') || f === 'icon.icns') fs.copyFileSync(path.join(resources, f), path.join(res, f));
  }
  const zip = path.join(ROOT, 'feed', `Plaud-${version}-arm64-mac.zip`);
  fs.mkdirSync(path.dirname(zip), { recursive: true });
  execFileSync('python3', [
    '-c',
    'import os,sys,zipfile\nroot,out=sys.argv[1],sys.argv[2]\nwith zipfile.ZipFile(out,"w",zipfile.ZIP_STORED) as z:\n  for d,_,fs in os.walk(root):\n    for f in fs:\n      p=os.path.join(d,f); z.write(p, os.path.relpath(p, root))',
    dir,
    zip,
  ]);
  const data = fs.readFileSync(zip);
  return { file: path.basename(zip), sha512: crypto.createHash('sha512').update(data).digest('base64'), size: data.length };
}

function publish(version, release, sha512 = release.sha512) {
  fs.writeFileSync(
    path.join(ROOT, 'feed', 'latest-mac.yml'),
    `version: ${version}\nfiles:\n  - url: ${release.file}\n    sha512: ${sha512}\n    size: ${release.size}\npath: ${release.file}\nsha512: ${sha512}\nreleaseDate: '2026-10-01T00:00:00.000Z'\n`,
  );
}

function serve() {
  const server = http.createServer((req, res) => {
    const file = path.join(ROOT, 'feed', decodeURIComponent(new URL(req.url, 'http://x').pathname.replace(/^\/desktop\/v2\//, '')));
    if (!file.startsWith(path.join(ROOT, 'feed')) || !fs.existsSync(file)) {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { 'content-length': fs.statSync(file).size });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

function runElectron({ electron, app }, feedUrl) {
  return new Promise((resolve, reject) => {
    const child = spawn(electron, ['--ozone-platform=headless', '--no-sandbox', '--disable-gpu', app], {
      // A private bus address keeps notifications and keyring prompts off the desktop.
      env: (() => {
        const env = { ...process.env, ...ENV, FEED_URL: feedUrl, PLAUD_LINUX_LOG_STDERR: '0', DBUS_SESSION_BUS_ADDRESS: 'unix:path=/nonexistent/bus' };
        delete env.ELECTRON_RUN_AS_NODE;
        return env;
      })(),
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    let err = '';
    child.stdout.on('data', (d) => (out += d));
    child.stderr.on('data', (d) => (err += d));
    child.on('error', reject);
    child.on('exit', (code) => {
      const line = out.split('\n').find((l) => l.startsWith('RESULT '));
      if (!line) reject(new Error(`no result (exit ${code})\n${out}\n${err.slice(-4000)}`));
      else resolve({ code, ...JSON.parse(line.slice(7)) });
    });
  });
}

/** Runs `fn` with the test's XDG directories in process.env (for in-process staging calls). */
function inEnv(fn) {
  const saved = { ...process.env };
  Object.assign(process.env, ENV);
  try {
    return fn();
  } finally {
    for (const k of Object.keys(ENV)) {
      if (saved[k] === undefined) delete process.env[k];
      else process.env[k] = saved[k];
    }
  }
}

const modeOf = (file) => fs.statSync(file).mode & 0o777;

test('the updater stages supported releases and refuses the others', { skip, timeout: 600_000 }, async (t) => {
  const env = await prepareElectron();
  const server = await serve();
  t.after(() => server.close());
  const feedUrl = `http://127.0.0.1:${server.address().port}/desktop/v2`;
  const dataDir = path.join(ENV.XDG_DATA_HOME, constants.DATA_DIR_NAME);
  const versionsDir = path.join(dataDir, 'versions');
  const stateDir = path.join(ENV.XDG_STATE_HOME, constants.DATA_DIR_NAME);
  const cacheDir = path.join(ENV.XDG_CACHE_HOME, constants.DATA_DIR_NAME);
  const staging = require('../../linux-runtime/staging');

  // 1. A supported release is downloaded, verified, patched and staged, in
  //    private directories even with umask 0000 (inherited by Electron and
  //    the staging child).
  publish('1.4.2', buildRelease('1.4.2'));
  const umask = process.umask(0o000);
  let first;
  try {
    first = await runElectron(env, feedUrl);
  } finally {
    process.umask(umask);
  }
  assert.equal(first.code, 0);
  assert.deepEqual(first.runs, [{ available: true, version: '1.4.2' }, { available: true, version: '1.4.2' }]);
  const names = first.events.map((e) => e.name);
  assert.equal(names[0], 'checking-for-update');
  assert.ok(names.includes('download-progress'));
  const downloaded = first.events.filter((e) => e.name === 'update-downloaded');
  assert.equal(downloaded.length, 2, 'the second check reuses the staged release');
  assert.equal(downloaded[0].file, path.join(versionsDir, '1.4.2', 'app.asar'));
  assert.equal(first.events.filter((e) => e.name === 'download-progress').length >= 1, true);

  const stage = JSON.parse(fs.readFileSync(path.join(versionsDir, '1.4.2', 'stage.json'), 'utf8'));
  assert.equal(stage.version, '1.4.2');
  assert.equal(stage.runtimeId, 'itest');
  const asarSha256 = crypto.createHash('sha256').update(fs.readFileSync(path.join(versionsDir, '1.4.2', 'app.asar'))).digest('hex');
  assert.deepEqual(stage.files, { 'app.asar': asarSha256 }, 'stage.json records the checksum of every staged file');
  for (const dir of [dataDir, versionsDir, path.join(versionsDir, '1.4.2'), cacheDir, path.join(cacheDir, 'downloads')]) {
    assert.equal(modeOf(dir), 0o700, `${dir} is private`);
  }
  assert.equal(modeOf(path.join(versionsDir, '1.4.2', 'stage.json')), 0o600);
  assert.equal(modeOf(path.join(versionsDir, '1.4.2', 'app.asar')) & 0o022, 0, 'the staged archive is writable only by its owner');
  const staged = AsarArchive.open(path.join(versionsDir, '1.4.2', 'app.asar'));
  try {
    const pkg = JSON.parse(staged.readFile('package.json').toString('utf8'));
    assert.equal(pkg.version, '1.4.2');
    assert.equal(pkg.main, patcher.LOADER);
    assert.equal(pkg.plaudLinux.runtimeId, 'itest');
  } finally {
    staged.close();
  }
  assert.deepEqual(fs.readdirSync(path.join(cacheDir, 'downloads')), [], 'downloads are removed after staging');

  // The loader would now boot it.
  inEnv(() => {
    assert.equal(staging.selectForBoot({ packagedVersion: pinned.version, runtimeId: 'itest' }).version, '1.4.2');
    assert.equal(staging.selectForBoot({ packagedVersion: pinned.version, runtimeId: 'other-build' }), null);
  });

  // 2. A release whose code no longer matches the patches is refused once.
  publish('1.4.3', buildRelease('1.4.3', { breakAnchor: true }));
  const second = await runElectron(env, feedUrl);
  assert.deepEqual(second.runs, [{ available: false, version: '1.4.3' }, { available: false, version: '1.4.3' }]);
  assert.equal(second.events.filter((e) => e.name === 'download-progress').length >= 1, true);
  assert.equal(second.events.filter((e) => e.name === 'update-available').length, 1, 'the refused release is not downloaded again');
  const refused = JSON.parse(fs.readFileSync(path.join(stateDir, 'refused-updates.json'), 'utf8'));
  assert.equal(refused['1.4.3'].runtimeId, 'itest');
  assert.match(refused['1.4.3'].reason, /recorder-addon-name/);
  assert.equal(modeOf(path.join(stateDir, 'refused-updates.json')), 0o600);
  assert.ok(!fs.existsSync(path.join(versionsDir, '1.4.3')));
  assert.ok(fs.existsSync(path.join(versionsDir, '1.4.2')), 'the staged release stays');

  // 3. A checksum mismatch is an error, not a refusal.
  const third = buildRelease('1.4.4');
  publish('1.4.4', third, crypto.createHash('sha512').update('other').digest('base64'));
  const mismatch = await runElectron(env, feedUrl);
  assert.equal(mismatch.runs[0].error, 'ERR_UPDATER_SHA512_MISMATCH');
  assert.ok(mismatch.events.some((e) => e.name === 'error'));
  assert.ok(!fs.existsSync(path.join(versionsDir, '1.4.4')));
  assert.equal(JSON.parse(fs.readFileSync(path.join(stateDir, 'refused-updates.json'), 'utf8'))['1.4.4'], undefined);

  // 4. A cache directory that is a symbolic link is refused before anything
  //    is downloaded into it.
  const elsewhere = path.join(ROOT, 'elsewhere');
  fs.mkdirSync(elsewhere, { mode: 0o700 });
  fs.rmSync(cacheDir, { recursive: true, force: true });
  fs.symlinkSync(elsewhere, cacheDir);
  publish('1.4.5', buildRelease('1.4.5'));
  const linked = await runElectron(env, feedUrl);
  assert.deepEqual(linked.runs, [{ error: 'ERR_UNSAFE_PATH' }, { error: 'ERR_UNSAFE_PATH' }]);
  assert.ok(linked.events.some((e) => e.name === 'error' && e.code === 'ERR_UNSAFE_PATH'));
  assert.ok(!linked.events.some((e) => e.name === 'download-progress'), 'nothing was downloaded');
  assert.deepEqual(fs.readdirSync(elsewhere), [], 'nothing was written through the link');
  assert.ok(!fs.existsSync(path.join(versionsDir, '1.4.5')));

  // 5. A staged archive modified after staging is not started.
  fs.appendFileSync(path.join(versionsDir, '1.4.2', 'app.asar'), 'x');
  inEnv(() => {
    const messages = [];
    assert.equal(staging.selectForBoot({ packagedVersion: pinned.version, runtimeId: 'itest', log: (m) => messages.push(m) }), null);
    assert.match(messages.join('\n'), /app\.asar does not match its SHA-256 checksum/);
  });
});
