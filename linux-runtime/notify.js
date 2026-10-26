'use strict';

// Desktop notifications for conditions the official UI cannot explain
// (missing keyring, missing ffmpeg codec, refused update). Each notice is
// shown at most once per `key`; keys are persisted in the state directory.

const { app, Notification, shell } = require('electron');
const { readState, writeState } = require('./staging');
const { createLogger } = require('./log');

const log = createLogger('notify');
const STATE_FILE = 'notices.json';

function readSeen() {
  try {
    return readState(STATE_FILE, {});
  } catch (error) {
    log.warn('reading the shown notices failed', { error: error.message });
    return {};
  }
}

function writeSeen(seen) {
  try {
    writeState(STATE_FILE, seen);
  } catch (error) {
    log.warn('remembering the shown notices failed', { error: error.message });
  }
}

/**
 * Shows a notification once per key. `url` is opened when it is clicked.
 * Returns true when shown.
 */
function noticeOnce(key, { title, body, url }) {
  const seen = readSeen();
  if (seen[key]) return false;
  const show = () => {
    if (!Notification.isSupported()) {
      log.warn('notifications unsupported; notice logged only', { key, title, body });
      return;
    }
    const n = new Notification({ title, body });
    if (url) n.on('click', () => void shell.openExternal(url));
    n.show();
  };
  if (app.isReady()) show();
  else app.whenReady().then(show);
  seen[key] = new Date().toISOString();
  writeSeen(seen);
  log.info('notice shown', { key });
  return true;
}

/** Allows the notice `key` to be shown again (the condition was resolved). */
function clearNotice(key) {
  const seen = readSeen();
  if (!seen[key]) return;
  delete seen[key];
  writeSeen(seen);
}

module.exports = { noticeOnce, clearNotice };
