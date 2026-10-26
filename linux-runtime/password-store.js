'use strict';

// safeStorage backend selection.
//
// The official app keeps its tokens in an electron-store whose values are
// encrypted with safeStorage. Chromium picks the Linux backend from
// XDG_CURRENT_DESKTOP: libsecret for GNOME-like desktops, KWallet for KDE and
// an unencrypted "basic_text" store for everything else (Hyprland, Sway, i3,
// ...). With basic_text safeStorage reports encryption as unavailable and
// encryptString() throws, which breaks sign-in.
//
// Before app 'ready' this module selects libsecret or KWallet explicitly when
// such a service is reachable on the session bus. After 'ready', if
// encryption is still unavailable, it enables Chromium's plain-text mode (the
// behaviour of Chrome on the same systems) and tells the user once.

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { app } = require('electron');
const { createLogger } = require('./log');

const log = createLogger('password-store');

// Desktops for which Chromium already selects a real backend
// (components/os_crypt/sync/key_storage_util_linux.cc).
const KNOWN_DESKTOP = /^(gnome|unity|pantheon|xfce|deepin|ukui|kde|x-cinnamon|cinnamon)$/i;

function dbusCall(method, args = []) {
  const res = spawnSync(
    'dbus-send',
    ['--session', '--print-reply', '--reply-timeout=1500', '--dest=org.freedesktop.DBus', '/org/freedesktop/DBus', `org.freedesktop.DBus.${method}`, ...args],
    { encoding: 'utf8', timeout: 2500 },
  );
  if (res.error || res.status !== 0) return null;
  return res.stdout;
}

function serviceFileExists(name) {
  const dirs = (process.env.XDG_DATA_DIRS || '/usr/local/share:/usr/share').split(':');
  dirs.unshift(process.env.XDG_DATA_HOME || path.join(process.env.HOME || '', '.local/share'));
  return dirs.some((dir) => dir && fs.existsSync(path.join(dir, 'dbus-1', 'services', `${name}.service`)));
}

/** True when `name` is owned or D-Bus-activatable on the session bus. */
function busNameAvailable(name) {
  const owner = dbusCall('NameHasOwner', [`string:${name}`]);
  if (owner !== null && /boolean true/.test(owner)) return true;
  const activatable = dbusCall('ListActivatableNames');
  if (activatable !== null) return activatable.includes(`"${name}"`);
  return serviceFileExists(name);
}

function currentDesktops() {
  return (process.env.XDG_CURRENT_DESKTOP || '').split(':').filter(Boolean);
}

/** Must run synchronously before app 'ready'. Returns the chosen store or null. */
function configureBeforeReady() {
  if (app.commandLine.hasSwitch('password-store')) {
    const store = app.commandLine.getSwitchValue('password-store');
    log.info('password store set on the command line', { store });
    return store;
  }
  const desktops = currentDesktops();
  if (desktops.some((d) => KNOWN_DESKTOP.test(d))) {
    log.info('using Chromium default password store for this desktop', { desktops });
    return null;
  }
  let store = null;
  if (busNameAvailable('org.freedesktop.secrets')) store = 'gnome-libsecret';
  else if (busNameAvailable('org.kde.kwalletd6')) store = 'kwallet6';
  else if (busNameAvailable('org.kde.kwalletd5')) store = 'kwallet5';
  if (store) {
    app.commandLine.appendSwitch('password-store', store);
    log.info('selected password store', { store, desktops });
  } else {
    log.warn('no Secret Service or KWallet on the session bus', { desktops });
  }
  return store;
}

/**
 * Must run in a whenReady() callback registered before the official app's.
 * Returns { backend, encrypted }.
 */
function ensureEncryptionAfterReady({ notify } = {}) {
  const { safeStorage } = require('electron');
  const backend = safeStorage.getSelectedStorageBackend?.() ?? 'unknown';
  if (safeStorage.isEncryptionAvailable()) {
    log.info('safeStorage encryption available', { backend });
    return { backend, encrypted: true };
  }
  safeStorage.setUsePlainTextEncryption(true);
  log.warn('no OS keyring available: credentials are stored without keyring protection', { backend });
  notify?.();
  return { backend, encrypted: false };
}

module.exports = { configureBeforeReady, ensureEncryptionAfterReady, busNameAvailable };
