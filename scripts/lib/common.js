'use strict';

// Shared helpers for the build and packaging scripts.

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const constants = require('../../linux-runtime/constants');

const REPO_ROOT = path.resolve(__dirname, '..', '..');
const CACHE_DIR = path.join(REPO_ROOT, '.cache');
const BUILD_DIR = path.join(REPO_ROOT, 'build');
const DIST_DIR = path.join(REPO_ROOT, 'dist');

/** Node arch names used by Electron and the packaging targets. */
const ARCHES = {
  x64: { electron: 'x64', deb: 'amd64', rpm: 'x86_64', pacman: 'x86_64', appimage: 'x86_64', uname: 'x86_64' },
  arm64: { electron: 'arm64', deb: 'arm64', rpm: 'aarch64', pacman: 'aarch64', appimage: 'aarch64', uname: 'aarch64' },
};

function resolveArch(requested) {
  const arch = requested ?? process.env.PLAUD_LINUX_ARCH ?? process.arch;
  if (!ARCHES[arch]) throw new Error(`unsupported architecture "${arch}" (supported: ${Object.keys(ARCHES).join(', ')})`);
  return arch;
}

function repoVersion() {
  return JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'package.json'), 'utf8')).version;
}

/** Minimal --flag / --key=value / --key value parser. */
function parseArgs(argv, { booleans = [] } = {}) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) {
      out._.push(a);
      continue;
    }
    const eq = a.indexOf('=');
    if (eq !== -1) {
      out[a.slice(2, eq)] = a.slice(eq + 1);
    } else if (booleans.includes(a.slice(2)) || i + 1 >= argv.length || argv[i + 1].startsWith('--')) {
      out[a.slice(2)] = true;
    } else {
      out[a.slice(2)] = argv[++i];
    }
  }
  return out;
}

function log(scope, message) {
  process.stderr.write(`[${scope}] ${message}\n`);
}

/** Runs a command, inheriting stdio; throws with context on failure. */
function run(cmd, args, options = {}) {
  const res = spawnSync(cmd, args, { stdio: 'inherit', ...options });
  if (res.error) throw new Error(`failed to run ${cmd}: ${res.error.message}`);
  if (res.status !== 0) {
    throw new Error(`${cmd} ${args.join(' ')} exited with status ${res.status ?? res.signal}`);
  }
  return res;
}

/** Runs a command and returns trimmed stdout, or null when it fails. */
function capture(cmd, args, options = {}) {
  const res = spawnSync(cmd, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], ...options });
  if (res.error || res.status !== 0) return null;
  return res.stdout.trim();
}

function which(cmd) {
  for (const dir of (process.env.PATH ?? '').split(':')) {
    if (!dir) continue;
    const candidate = path.join(dir, cmd);
    try {
      fs.accessSync(candidate, fs.constants.X_OK);
      return candidate;
    } catch {
      // keep looking
    }
  }
  return null;
}

function rmrf(p) {
  fs.rmSync(p, { recursive: true, force: true });
}

function writeFileAtomic(file, data, mode) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const tmp = `${file}.tmp-${process.pid}`;
  fs.writeFileSync(tmp, data, mode === undefined ? undefined : { mode });
  fs.renameSync(tmp, file);
}

/** Copies a directory tree preserving modes and symlinks. */
function copyTree(src, dest) {
  fs.cpSync(src, dest, { recursive: true, verbatimSymlinks: true, preserveTimestamps: true });
}

/** Expands @NAME@ placeholders; fails on unknown placeholders. */
function renderTemplate(text, vars) {
  return text.replace(/@([A-Z0-9_]+)@/g, (m, key) => {
    if (!(key in vars)) throw new Error(`template placeholder ${m} has no value`);
    return String(vars[key]);
  });
}

module.exports = {
  REPO_ROOT,
  CACHE_DIR,
  BUILD_DIR,
  DIST_DIR,
  ARCHES,
  constants,
  resolveArch,
  repoVersion,
  parseArgs,
  log,
  run,
  capture,
  which,
  rmrf,
  writeFileAtomic,
  copyTree,
  renderTemplate,
};
