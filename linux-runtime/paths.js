'use strict';

// XDG Base Directory locations used by the Linux runtime.

const os = require('node:os');
const path = require('node:path');
const { DATA_DIR_NAME, RESOURCES_SUBDIR } = require('./constants');

function xdg(envName, fallback) {
  const value = process.env[envName];
  // The spec requires absolute paths; relative values must be ignored.
  return value && path.isAbsolute(value) ? value : path.join(os.homedir(), fallback);
}

const dataHome = () => xdg('XDG_DATA_HOME', '.local/share');
const stateHome = () => xdg('XDG_STATE_HOME', '.local/state');
const configHome = () => xdg('XDG_CONFIG_HOME', '.config');
const cacheHome = () => xdg('XDG_CACHE_HOME', '.cache');

module.exports = {
  dataHome,
  stateHome,
  configHome,
  cacheHome,
  /** Per-user data of this build (staged updates, per-user install). */
  dataDir: () => path.join(dataHome(), DATA_DIR_NAME),
  /** Re-patched official releases staged by the updater. */
  versionsDir: () => path.join(dataHome(), DATA_DIR_NAME, 'versions'),
  /** Boot bookkeeping (boot attempts, refused updates). */
  stateDir: () => path.join(stateHome(), DATA_DIR_NAME),
  /** Build metadata, bundled patcher and runtime sources. */
  resourcesDir: () => path.join(process.resourcesPath, RESOURCES_SUBDIR),
  /** Same directory the official app uses for userData (appData/Plaud). */
  plaudUserData: () => path.join(configHome(), 'Plaud'),
};
