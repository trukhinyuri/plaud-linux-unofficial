'use strict';

// Bookkeeping for re-patched official releases staged by the updater:
//   $XDG_DATA_HOME/plaud-linux/versions/<version>/{app.asar,stage.json}
// and for boot attempts of a staged release and other runtime state:
//   $XDG_STATE_HOME/plaud-linux/{boot.json,bad-versions.json,refused-updates.json,...}
// The loader starts the newest staged release that was produced by this very
// build (same runtime ID); a release that twice fails to reach a healthy state
// is marked bad and the packaged release is used instead.
//
// All of these directories are private (see secure-fs.js). stage.json, the
// manifest the patcher writes last, records the SHA-256 of every file of the
// staged release; a release is started only when its directory passes the
// checks and every file matches.

const path = require('node:path');
const paths = require('./paths');
const semver = require('./semver');
const secure = require('./secure-fs');

const { fs } = secure;

const MAX_BOOT_ATTEMPTS = 2;
const STAGE_FILE = 'stage.json';

function readJson(file, fallback) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return fallback;
  }
}

function parseJson(data, fallback) {
  if (data === null) return fallback;
  try {
    return JSON.parse(data.toString('utf8'));
  } catch {
    return fallback;
  }
}

/** Reads the JSON state file `name`; `fallback` when it does not exist. Throws UnsafePathError. */
function readState(name, fallback) {
  try {
    secure.assertPrivateDir(paths.stateDir());
  } catch (error) {
    if (error.code === 'ENOENT') return fallback;
    throw error;
  }
  return parseJson(secure.readPrivateFile(path.join(paths.stateDir(), name)), fallback);
}

/** Writes the JSON state file `name`. Throws UnsafePathError. */
function writeState(name, value) {
  const dir = secure.ensurePrivateDir(paths.stateDir(), paths.stateHome());
  secure.writePrivateFile(path.join(dir, name), `${JSON.stringify(value, null, 2)}\n`);
}

function readBuildInfo(resourcesDir = paths.resourcesDir()) {
  const info = readJson(path.join(resourcesDir, 'build-info.json'), null);
  if (!info || typeof info.runtimeId !== 'string') {
    throw new Error(`missing or invalid build-info.json in ${resourcesDir}`);
  }
  return info;
}

/**
 * Lists staged releases (newest first) whose directory and stage.json pass
 * the checks. Returns [] when nothing was staged yet; throws UnsafePathError
 * when the data or versions directory fails them. `log` receives the reasons
 * entries were skipped.
 */
function listStaged(versionsDir = paths.versionsDir(), log = () => {}) {
  try {
    secure.assertPrivateDir(path.dirname(versionsDir));
    secure.assertPrivateDir(versionsDir);
  } catch (error) {
    if (error.code === 'ENOENT') return [];
    throw error;
  }
  const out = [];
  for (const name of fs.readdirSync(versionsDir)) {
    if (name.startsWith('.') || !semver.parse(name)) continue;
    const dir = path.join(versionsDir, name);
    let stage;
    try {
      secure.assertPrivateDir(dir);
      stage = parseJson(secure.readPrivateFile(path.join(dir, STAGE_FILE)), null);
    } catch (error) {
      log(`ignoring staged ${name}: ${error.message}`);
      continue;
    }
    if (!stage || stage.version !== name) continue;
    out.push({ version: name, dir, asar: path.join(dir, 'app.asar'), stage });
  }
  return out.sort((a, b) => semver.compare(b.version, a.version));
}

/**
 * Throws UnsafePathError unless the files of a staged release are exactly the
 * ones its stage.json lists, with the same SHA-256.
 */
function verifyStaged({ dir, stage }) {
  const expected = stage.files;
  if (!expected || typeof expected !== 'object' || typeof expected['app.asar'] !== 'string') {
    throw new secure.UnsafePathError(path.join(dir, STAGE_FILE), 'records no file checksums');
  }
  const actual = secure.hashTree(dir, { exclude: [STAGE_FILE] });
  for (const name of new Set([...Object.keys(expected), ...Object.keys(actual)])) {
    if (expected[name] === actual[name]) continue;
    let reason = 'does not match its SHA-256 checksum';
    if (actual[name] === undefined) reason = 'is missing';
    else if (expected[name] === undefined) reason = 'is not part of the staged release';
    throw new secure.UnsafePathError(path.join(dir, name), reason);
  }
}

/** The verified staged release `version` of build `runtimeId`, or null. */
function findStaged({ version, runtimeId, versionsDir = paths.versionsDir(), log = () => {} }) {
  const entry = listStaged(versionsDir, log).find((s) => s.version === version && s.stage.runtimeId === runtimeId);
  if (!entry) return null;
  try {
    verifyStaged(entry);
    return entry;
  } catch (error) {
    log(`staged ${version} is not usable: ${error.message}`);
    return null;
  }
}

function markBad(version, reason) {
  const bad = readState('bad-versions.json', {});
  bad[version] = { reason, at: new Date().toISOString() };
  writeState('bad-versions.json', bad);
}

function pick({ packagedVersion, runtimeId, versionsDir, log }) {
  const staged = listStaged(versionsDir, log);
  if (!staged.length) return null;
  const bad = readState('bad-versions.json', {});
  const boot = readState('boot.json', null);
  for (const candidate of staged) {
    if (!semver.gt(candidate.version, packagedVersion)) continue;
    if (candidate.stage.runtimeId !== runtimeId) continue;
    if (bad[candidate.version]) continue;
    const attempts = boot && boot.pending && boot.version === candidate.version ? boot.attempts : 0;
    if (attempts >= MAX_BOOT_ATTEMPTS) {
      log(`staged ${candidate.version} did not start successfully ${attempts} times; marking it bad`);
      markBad(candidate.version, `failed to start ${attempts} times`);
      continue;
    }
    try {
      verifyStaged(candidate);
    } catch (error) {
      log(`staged ${candidate.version} is not usable: ${error.message}`);
      continue;
    }
    writeState('boot.json', {
      version: candidate.version,
      attempts: attempts + 1,
      pending: true,
      startedAt: new Date().toISOString(),
    });
    return candidate;
  }
  return null;
}

/**
 * Picks the staged release to run, recording a boot attempt for it.
 * Returns null when the packaged release should run, also when the staged
 * releases or the boot bookkeeping fail the checks (the reason is logged).
 */
function selectForBoot({ packagedVersion, runtimeId, versionsDir = paths.versionsDir(), log = () => {} }) {
  try {
    return pick({ packagedVersion, runtimeId, versionsDir, log });
  } catch (error) {
    log(`staged releases not used: ${error.message}`);
    return null;
  }
}

/**
 * Called by the runtime once the app is up; clears the pending boot attempt.
 * Returns true when it did.
 */
function markHealthy(version) {
  const boot = readState('boot.json', null);
  if (!boot || boot.version !== version || !boot.pending) return false;
  writeState('boot.json', { ...boot, pending: false, healthyAt: new Date().toISOString() });
  return true;
}

/** Removes staged releases that can no longer be used by this build. */
function pruneStaged({ packagedVersion, runtimeId, keepVersion, versionsDir = paths.versionsDir() }) {
  const removed = [];
  const staged = listStaged(versionsDir);
  const newestUsable = staged.find((s) => s.stage.runtimeId === runtimeId && semver.gt(s.version, packagedVersion));
  for (const s of staged) {
    if (s.version === keepVersion || s === newestUsable) continue;
    fs.rmSync(s.dir, { recursive: true, force: true });
    removed.push(s.version);
  }
  // Leftovers of interrupted staging runs (listStaged checked the directory).
  if (fs.existsSync(versionsDir)) {
    for (const entry of fs.readdirSync(versionsDir)) {
      if (!entry.startsWith('.tmp-')) continue;
      const full = path.join(versionsDir, entry);
      const age = Date.now() - fs.lstatSync(full).mtimeMs;
      if (age > 6 * 3600 * 1000) fs.rmSync(full, { recursive: true, force: true });
    }
  }
  return removed;
}

function recordRefused(version, runtimeId, reason) {
  const refused = readState('refused-updates.json', {});
  const first = refused[version]?.runtimeId !== runtimeId;
  refused[version] = { runtimeId, reason, at: new Date().toISOString() };
  writeState('refused-updates.json', refused);
  return first;
}

function isRefused(version, runtimeId) {
  return readState('refused-updates.json', {})[version]?.runtimeId === runtimeId;
}

module.exports = {
  STAGE_FILE,
  readState,
  writeState,
  readBuildInfo,
  listStaged,
  verifyStaged,
  findStaged,
  selectForBoot,
  markHealthy,
  markBad,
  pruneStaged,
  recordRefused,
  isRefused,
};
