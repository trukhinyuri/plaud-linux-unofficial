'use strict';

// app.relaunch() re-executes process.execPath after the current process
// exits. Inside an AppImage, process.execPath lives on the image's FUSE mount,
// which disappears together with the current process, so Electron's relauncher
// cannot start it. For AppImages the relaunch is delegated to a detached
// /bin/sh that waits for this process to exit and then starts $APPIMAGE.

const { spawn } = require('node:child_process');
const { app } = require('electron');
const { createLogger } = require('./log');

const log = createLogger('relaunch');
let installed = false;

const WAITER = 'while kill -0 "$0" 2>/dev/null; do sleep 0.2; done; exec "$@"';

function install() {
  if (installed || !process.env.APPIMAGE) return;
  installed = true;
  let scheduled = false;
  const original = app.relaunch.bind(app);
  app.relaunch = (options = {}) => {
    if (scheduled) return;
    scheduled = true;
    const execPath = options.execPath ?? process.env.APPIMAGE;
    const args = options.args ?? process.argv.slice(1);
    try {
      const child = spawn('/bin/sh', ['-c', WAITER, String(process.pid), execPath, ...args], {
        detached: true,
        stdio: 'ignore',
      });
      child.unref();
      log.info('relaunch scheduled through $APPIMAGE', { execPath });
    } catch (error) {
      log.error('cannot schedule AppImage relaunch; using Electron relaunch', { error });
      original(options);
    }
  };
}

module.exports = { install };
