'use strict';

// Linux initialisation, then the official main process.
//
// Runs inside the selected (packaged or staged) archive, before any official
// module. Everything that must happen before app 'ready' is synchronous here;
// the whenReady() callback registered below runs before the official app's
// own whenReady() callbacks because it is registered first.

const path = require('node:path');
const { app } = require('electron');
const constants = require('./constants');
const { createLogger, logFile } = require('./log');
const polyfills = require('./polyfills');
const passwordStore = require('./password-store');
const staging = require('./staging');
const { noticeOnce } = require('./notify');

const log = createLogger('main');
const pkg = require('../package.json');
const activeRelease = process.env.PLAUD_LINUX_ACTIVE_RELEASE || 'packaged';

function onReady() {
  passwordStore.ensureEncryptionAfterReady({
    notify: () =>
      noticeOnce('plaintext-credentials', {
        title: 'Plaud: no keyring found',
        body: 'Sign-in tokens are stored without keyring protection. Install and start a Secret Service (GNOME Keyring, KeePassXC or KWallet), then restart Plaud.',
      }),
  });
  polyfills.installAfterReady();
  require('./ffmpeg').checkInBackground();

  // A staged release counts as healthy once it has run for a while, or when
  // it quits normally after reaching 'ready'.
  if (activeRelease !== 'packaged') {
    const healthy = () => {
      try {
        if (staging.markHealthy(activeRelease)) log.info('staged release marked healthy', { version: activeRelease });
      } catch (error) {
        log.warn('marking the staged release healthy failed', { version: activeRelease, error: error.message });
      }
    };
    setTimeout(healthy, 20_000).unref();
    app.once('will-quit', healthy);
  }
  // Development runs (scripts/dev-run.js) have no build-info.json and must
  // not touch the staged releases of an installed build.
  if (!app.isPackaged) return;
  setTimeout(() => {
    try {
      const buildInfo = staging.readBuildInfo();
      const removed = staging.pruneStaged({
        packagedVersion: buildInfo.upstreamVersion,
        runtimeId: buildInfo.runtimeId,
        keepVersion: activeRelease,
      });
      if (removed.length) log.info('removed obsolete staged releases', { removed });
    } catch (error) {
      log.warn('pruning staged releases failed', { error: error.message });
    }
  }, 60_000).unref();
}

function start() {
  log.info('starting', {
    plaudVersion: app.getVersion(),
    release: activeRelease,
    runtimeId: pkg.plaudLinux?.runtimeId,
    electron: process.versions.electron,
    arch: process.arch,
    session: process.env.XDG_SESSION_TYPE,
    desktop: process.env.XDG_CURRENT_DESKTOP,
    ozone: polyfills.ozonePlatform(),
    appImage: Boolean(process.env.APPIMAGE),
    log: logFile(),
  });

  polyfills.installEarly();
  passwordStore.configureBeforeReady();
  require('./global-shortcuts').install();
  if (process.env.APPIMAGE) require('./desktop-integration').ensure();
  app.whenReady().then(onReady);

  const upstreamMain = pkg.plaudLinux?.upstreamMain ?? './out-global-online/main/index.js';
  log.info('loading the official main process', { main: upstreamMain, appId: constants.APP_ID });
  require(path.join(__dirname, '..', upstreamMain));
}

require('./relaunch').install();

if (process.argv.includes('--plaud-linux-remove-desktop-integration')) {
  require('./desktop-integration').remove();
  app.exit(0);
} else {
  start();
}
