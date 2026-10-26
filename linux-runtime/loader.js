'use strict';

// Entry point of the patched app (package.json "main").
//
// The packaged app.asar was patched at build time. The Linux updater stages
// newer official releases, re-patched by the same patcher, under
// $XDG_DATA_HOME/plaud-linux/versions/. This loader starts the newest staged
// release that belongs to this build and passes the checks of staging.js
// (private directories, files matching the SHA-256 checksums of stage.json),
// or the packaged one, then hands over to linux-runtime/main.js of the
// selected archive.

const path = require('node:path');
const { app } = require('electron');
const { createLogger } = require('./log');
const staging = require('./staging');

require('./relaunch').install();

const log = createLogger('loader');

function selectStaged() {
  if (!app.isPackaged) {
    log.info('development run: staged releases disabled');
    return null;
  }
  if (process.env.PLAUD_LINUX_NO_STAGED === '1' || process.argv.includes('--plaud-linux-packaged')) {
    log.info('staged releases disabled for this run');
    return null;
  }
  let buildInfo;
  try {
    buildInfo = staging.readBuildInfo();
  } catch (error) {
    log.warn('build metadata unavailable; running the packaged release', { error: error.message });
    return null;
  }
  return staging.selectForBoot({
    packagedVersion: app.getVersion(),
    runtimeId: buildInfo.runtimeId,
    log: (message) => log.warn(message),
  });
}

function run() {
  const packagedVersion = app.getVersion();
  const staged = selectStaged();
  if (!staged) {
    process.env.PLAUD_LINUX_ACTIVE_RELEASE = 'packaged';
    require('./main.js');
    return;
  }
  log.info('starting staged release', { version: staged.version, packagedVersion, asar: staged.asar });
  process.env.PLAUD_LINUX_ACTIVE_RELEASE = staged.version;
  app.setAppPath(staged.asar);
  app.setVersion(staged.version);
  try {
    require(path.join(staged.asar, 'linux-runtime', 'main.js'));
  } catch (error) {
    // Module initialisation of the staged release failed. Its partially
    // registered handlers make an in-process fallback unsafe: mark it bad and
    // restart into the packaged release.
    log.error('staged release failed to load; restarting with the packaged release', { version: staged.version, error });
    try {
      staging.markBad(staged.version, `load error: ${error?.message ?? error}`);
    } catch (markError) {
      log.error('marking the staged release bad failed', { version: staged.version, error: markError.message });
    }
    app.relaunch({ args: [...process.argv.slice(1), '--plaud-linux-packaged'] });
    app.exit(0);
  }
}

run();
