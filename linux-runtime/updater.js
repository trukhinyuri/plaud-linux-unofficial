'use strict';

// Linux replacement for electron-updater's autoUpdater, with the interface the
// official update service uses (misc-*.js): channel, allowDowngrade,
// allowPrerelease, currentVersion, setFeedURL(), checkForUpdates(),
// quitAndInstall() and the checking-for-update / update-available /
// download-progress / update-downloaded / update-not-available / error events.
//
// Plaud publishes no Linux build. Updates therefore come from the official
// macOS feed: the zip is downloaded, verified against the feed's SHA-512,
// re-patched by the patcher shipped with this build and staged for the
// loader. Releases that the patcher refuses (moved anchors, unknown native
// API) are not installed; the user is told once. Downloads and staged
// releases live in private directories (secure-fs.js); when one of them
// fails the checks, the update check fails and nothing is written there.

const EventEmitter = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { app, net } = require('electron');
const paths = require('./paths');
const constants = require('./constants');
const semver = require('./semver');
const feed = require('./feed');
const staging = require('./staging');
const secure = require('./secure-fs');
const { noticeOnce } = require('./notify');
const { createLogger } = require('./log');

const log = createLogger('updater');

class UpdaterError extends Error {
  constructor(message, code) {
    super(message);
    this.code = code;
  }
}

async function fetchText(url) {
  const res = await net.fetch(url, { cache: 'no-store' });
  if (!res.ok) throw new UpdaterError(`GET ${url} failed with HTTP ${res.status}`, 'ERR_UPDATER_HTTP');
  return res.text();
}

/** Private directory for release downloads, emptied of leftovers of interrupted runs. */
function downloadsDir() {
  const dir = secure.ensurePrivateDir(path.join(paths.cacheHome(), constants.DATA_DIR_NAME, 'downloads'), paths.cacheHome());
  for (const entry of fs.readdirSync(dir)) fs.rmSync(path.join(dir, entry), { recursive: true, force: true });
  return dir;
}

/** Downloads `url` into a new private file in `dir`; returns its path. */
async function download(url, dir, name, expectedSize, onProgress) {
  const res = await net.fetch(url, { cache: 'no-store' });
  if (!res.ok || !res.body) throw new UpdaterError(`GET ${url} failed with HTTP ${res.status}`, 'ERR_UPDATER_HTTP');
  const total = Number(res.headers.get('content-length')) || expectedSize || 0;
  const { fd, file } = secure.createPrivateFile(dir, path.basename(name));
  try {
    const out = fs.createWriteStream(file, { fd });
    const started = Date.now();
    let transferred = 0;
    let lastReport = 0;
    try {
      for await (const chunk of res.body) {
        transferred += chunk.length;
        if (!out.write(chunk)) await new Promise((resolve) => out.once('drain', resolve));
        const now = Date.now();
        if (now - lastReport >= 500 || (total && transferred === total)) {
          lastReport = now;
          const seconds = Math.max((now - started) / 1000, 0.001);
          onProgress({
            total,
            transferred,
            percent: total ? (transferred / total) * 100 : 0,
            bytesPerSecond: Math.round(transferred / seconds),
            delta: chunk.length,
          });
        }
      }
    } finally {
      await new Promise((resolve, reject) => out.end((err) => (err ? reject(err) : resolve())));
    }
    if (expectedSize && transferred !== expectedSize) {
      throw new UpdaterError(`download size mismatch: expected ${expectedSize}, got ${transferred}`, 'ERR_UPDATER_SIZE');
    }
    return file;
  } catch (error) {
    fs.rmSync(file, { force: true });
    throw error;
  }
}

/** Runs patcher/cli.js stage in a child Electron process acting as Node. */
function runStage({ zip, info, resourcesDir, versionsDir }) {
  const cli = path.join(resourcesDir, 'patcher', 'cli.js');
  const args = [cli, 'stage', '--zip', zip, '--sha512', info.sha512, '--version', info.version, '--versions-dir', versionsDir, '--resources-dir', resourcesDir];
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, args, {
      env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => (stdout += d));
    child.stderr.on('data', (d) => (stderr += d));
    child.on('error', reject);
    child.on('close', (code) => {
      const last = stdout.trim().split('\n').at(-1) ?? '';
      let result;
      try {
        result = JSON.parse(last);
      } catch {
        reject(new UpdaterError(`staging exited with ${code} without a result: ${stderr.trim().slice(-2000)}`, 'ERR_UPDATER_STAGE'));
        return;
      }
      if (stderr.trim()) log.info('staging output', { stderr: stderr.trim().slice(-4000) });
      resolve(result);
    });
  });
}

class LinuxAutoUpdater extends EventEmitter {
  constructor() {
    super();
    this.channel = 'latest';
    this.allowDowngrade = false;
    this.allowPrerelease = false;
    this.forceDevUpdateConfig = false;
    this.feedUrl = null;
    this.running = null;
  }

  get currentVersion() {
    const version = app.getVersion();
    return { version, ...semver.parse(version) };
  }

  setFeedURL(options) {
    const url = typeof options === 'string' ? options : options?.url;
    if (!url) throw new UpdaterError('setFeedURL requires a URL', 'ERR_UPDATER_INVALID_FEED');
    this.feedUrl = url.replace(/\/+$/, '');
    log.info('feed configured', { url: this.feedUrl });
  }

  getFeedURL() {
    return this.feedUrl;
  }

  checkForUpdates() {
    // Same contract as electron-updater: unpackaged (development) runs do not update.
    if (!app.isPackaged && !this.forceDevUpdateConfig) {
      log.info('skipping update check: application is not packaged');
      return Promise.resolve(null);
    }
    if (!this.running) {
      this.running = this.#check().finally(() => {
        this.running = null;
      });
    }
    return this.running;
  }

  async #check() {
    if (!this.feedUrl) {
      const error = new UpdaterError('update feed URL is not set', 'ERR_UPDATER_INVALID_FEED');
      this.emit('error', error);
      throw error;
    }
    this.emit('checking-for-update');
    try {
      return await this.#run();
    } catch (error) {
      log.error('update check failed', { error });
      this.emit('error', error);
      throw error;
    }
  }

  async #run() {
    const resourcesDir = paths.resourcesDir();
    const buildInfo = staging.readBuildInfo(resourcesDir);
    const channel = this.channel || 'latest';
    const feedFile = `${this.feedUrl}/${feed.macFeedFileName(channel)}`;
    const info = feed.selectMacZip(feed.parseFeedYaml(await fetchText(feedFile)));
    const current = app.getVersion();
    const updateInfo = {
      version: info.version,
      releaseDate: info.releaseDate,
      path: info.file,
      sha512: info.sha512,
      files: [{ url: info.file, sha512: info.sha512, size: info.size }],
    };
    const newer = semver.gt(info.version, current);
    const allowed = newer || (this.allowDowngrade && semver.lt(info.version, current));
    if (!allowed || (!this.allowPrerelease && semver.isPrerelease(info.version))) {
      log.info('no update', { current, latest: info.version, channel });
      this.emit('update-not-available', updateInfo);
      return { updateInfo, isUpdateAvailable: false };
    }
    if (staging.isRefused(info.version, buildInfo.runtimeId)) {
      log.info('release was refused earlier by this build', { version: info.version });
      this.emit('update-not-available', updateInfo);
      return { updateInfo, isUpdateAvailable: false };
    }

    this.emit('update-available', updateInfo);
    const versionsDir = secure.ensurePrivateDir(paths.versionsDir(), paths.dataHome());
    const alreadyStaged = staging.findStaged({
      version: info.version,
      runtimeId: buildInfo.runtimeId,
      versionsDir,
      log: (message) => log.warn(message),
    });
    if (alreadyStaged) {
      log.info('release already staged', { version: info.version });
      this.emit('update-downloaded', { ...updateInfo, downloadedFile: alreadyStaged.asar });
      return { updateInfo, isUpdateAvailable: true };
    }

    const dir = downloadsDir();
    const url = feed.resolveFileUrl(this.feedUrl, info.file);
    log.info('downloading release', { version: info.version, url });
    const zip = await download(url, dir, info.file, info.size, (progress) => this.emit('download-progress', progress));
    try {
      const result = await runStage({ zip, info, resourcesDir, versionsDir });
      if (!result.ok) {
        log.warn('release refused by the patcher', { version: info.version, result });
        if (result.code === 'SHA512_MISMATCH' || result.code === 'IO_ERROR') {
          throw new UpdaterError(result.message, `ERR_UPDATER_${result.code}`);
        }
        const firstTime = staging.recordRefused(info.version, buildInfo.runtimeId, result.message);
        if (firstTime) {
          noticeOnce(`refused-${info.version}-${buildInfo.runtimeId}`, {
            title: `Plaud ${info.version} is not supported yet`,
            body: `This Linux build keeps running Plaud ${current}. Update ${constants.PACKAGE_NAME} to get Plaud ${info.version}.`,
            url: constants.PROJECT_URL,
          });
        }
        this.emit('update-not-available', updateInfo);
        return { updateInfo, isUpdateAvailable: false };
      }
      staging.pruneStaged({ packagedVersion: buildInfo.upstreamVersion, runtimeId: buildInfo.runtimeId, keepVersion: current, versionsDir });
      log.info('release staged', { version: info.version, dir: result.dir });
      this.emit('update-downloaded', { ...updateInfo, downloadedFile: path.join(result.dir, 'app.asar') });
      return { updateInfo, isUpdateAvailable: true };
    } finally {
      fs.rmSync(zip, { force: true });
    }
  }

  /** Restarts into the staged release (the loader picks it up at start). */
  quitAndInstall(isSilent = false, isForceRunAfter = true) {
    log.info('restarting into the staged release', { isSilent, isForceRunAfter });
    if (isForceRunAfter !== false) app.relaunch();
    app.quit();
  }
}

const autoUpdater = new LinuxAutoUpdater();

module.exports = { autoUpdater, LinuxAutoUpdater, UpdaterError };
