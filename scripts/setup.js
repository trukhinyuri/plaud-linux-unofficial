#!/usr/bin/env node
'use strict';

// Builds Plaud Desktop for Linux on this machine and installs it. This is the
// part of the installer packages (scripts/package-installer.js) that runs
// after installation; the packages themselves contain only this project's
// code. Started by packaging/installer/plaud-linux-setup with the verified
// stock Electron as the Node runtime (ELECTRON_RUN_AS_NODE=1), because the
// distributions' Node.js is too old on Ubuntu 22.04/24.04 and RHEL 9.
//
//   setup.js install --mode user|system --work-dir <dir> --electron-zip <zip>
//                    [--official-zip <zip>] [--prefix <dir>] [--share-dir <dir>]
//
// It downloads the official Plaud release pinned in official.json, verifies
// its SHA-512, patches it and combines it with the verified Electron and the
// prebuilt Linux addons shipped in the package (scripts/build.js), then
//   --mode user    installs to ~/.local/share/plaud-linux/app (scripts/install);
//   --mode system  installs to <prefix> (default /opt/plaud-linux) with the
//                  setuid-root chrome-sandbox, and the icons and AppStream
//                  metadata rendered from the official icon to <share-dir>
//                  (default /usr/local/share).
// The build directory, which holds a copy of Plaud's code, is removed
// afterwards; the verified downloads stay in the work directory's cache.

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

// Under Electron, Node's fs treats *.asar files as directories; the build
// writes and replaces such files (app.asar, default_app.asar) as plain files.
if (process.versions.electron) process.noAsar = true;

const DEFAULT_PREFIX = '/opt/plaud-linux';
const DEFAULT_SHARE = '/usr/local/share';

function parse(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) out._.push(a);
    else if (i + 1 < argv.length && !argv[i + 1].startsWith('--')) out[a.slice(2)] = argv[++i];
    else out[a.slice(2)] = true;
  }
  return out;
}

function need(args, key) {
  if (typeof args[key] !== 'string' || args[key] === '') throw new Error(`missing --${key}`);
  return args[key];
}

/** Points the build scripts at the work directory; must run before they are loaded. */
function useWorkDir(workDir) {
  process.env.PLAUD_LINUX_CACHE_DIR = path.join(workDir, 'cache');
  process.env.PLAUD_LINUX_BUILD_DIR = path.join(workDir, 'build');
  process.env.PLAUD_LINUX_DIST_DIR = path.join(workDir, 'dist');
}

/** Files of the share/ tree the system install owns (relative to share-dir). */
function shareFiles(shareOut) {
  const files = [];
  const walk = (rel) => {
    for (const entry of fs.readdirSync(path.join(shareOut, rel), { withFileTypes: true })) {
      const r = path.join(rel, entry.name);
      if (entry.isDirectory()) walk(r);
      else files.push(r);
    }
  };
  walk('');
  // The packages ship their own desktop entry (Exec=plaud-linux).
  return files.filter((f) => !f.startsWith(`applications${path.sep}`));
}

function installSystem({ appDir, shareOut, prefix, shareDir, constants }) {
  if (typeof process.getuid === 'function' && process.getuid() !== 0) {
    throw new Error('--mode system must run as root (sudo plaud-linux-setup --system)');
  }
  const parent = path.dirname(prefix);
  fs.mkdirSync(parent, { recursive: true });
  const fresh = `${prefix}.new-${process.pid}`;
  const old = `${prefix}.old-${process.pid}`;
  fs.rmSync(fresh, { recursive: true, force: true });
  fs.cpSync(appDir, fresh, { recursive: true, verbatimSymlinks: true });
  // Root-owned tree; the Chromium sandbox helper must be setuid root.
  const sandbox = path.join(fresh, 'chrome-sandbox');
  fs.chownSync(sandbox, 0, 0);
  fs.chmodSync(sandbox, 0o4755);
  fs.chmodSync(fresh, 0o755);
  // Swap atomically: a running app keeps its (renamed, then deleted) files.
  if (fs.existsSync(prefix)) fs.renameSync(prefix, old);
  fs.renameSync(fresh, prefix);
  fs.rmSync(old, { recursive: true, force: true });

  const installed = [];
  for (const rel of shareFiles(shareOut)) {
    const dest = path.join(shareDir, rel);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.copyFileSync(path.join(shareOut, rel), dest);
    fs.chmodSync(dest, 0o644);
    installed.push(dest);
  }
  // Recorded for plaud-linux-setup --remove.
  fs.writeFileSync(path.join(prefix, 'resources', constants.RESOURCES_SUBDIR, 'installed-files'), `${installed.join('\n')}\n`);
  const iconDir = path.join(shareDir, 'icons', 'hicolor');
  if (fs.existsSync(path.join(iconDir, 'index.theme'))) {
    spawnSync('gtk-update-icon-cache', ['-q', '-t', '-f', iconDir], { stdio: 'ignore' });
  }
  return prefix;
}

function installUser({ buildOut, libDir }) {
  const res = spawnSync('sh', [path.join(libDir, 'scripts', 'install'), '--from', buildOut], { stdio: 'inherit' });
  if (res.error) throw new Error(`failed to run scripts/install: ${res.error.message}`);
  if (res.status !== 0) throw new Error(`scripts/install exited with status ${res.status ?? res.signal}`);
}

async function main() {
  const args = parse(process.argv.slice(2));
  const command = args._[0];
  if (command !== 'install') throw new Error('usage: setup.js install --mode user|system --work-dir <dir> --electron-zip <zip>');
  const mode = need(args, 'mode');
  if (mode !== 'user' && mode !== 'system') throw new Error(`--mode must be user or system (got ${mode})`);
  const workDir = path.resolve(need(args, 'work-dir'));
  useWorkDir(workDir);

  const libDir = path.resolve(__dirname, '..');
  const { build } = require('./build');
  const { constants, log, rmrf } = require('./lib/common');
  const result = await build({
    skipNativeBuild: true,
    electronZip: path.resolve(need(args, 'electron-zip')),
    officialZip: typeof args['official-zip'] === 'string' ? path.resolve(args['official-zip']) : undefined,
  });
  try {
    if (mode === 'system') {
      const prefix = path.resolve(typeof args.prefix === 'string' ? args.prefix : DEFAULT_PREFIX);
      const shareDir = path.resolve(typeof args['share-dir'] === 'string' ? args['share-dir'] : DEFAULT_SHARE);
      installSystem({ appDir: result.appDir, shareOut: result.shareDir, prefix, shareDir, constants });
      log('setup', `installed Plaud ${result.buildInfo.upstreamVersion} to ${prefix}`);
    } else {
      installUser({ buildOut: result.outDir, libDir });
    }
  } finally {
    rmrf(process.env.PLAUD_LINUX_BUILD_DIR);
    // Keep only the verified downloads; the extracted official resources are
    // recreated from the zip when needed.
    rmrf(path.join(process.env.PLAUD_LINUX_CACHE_DIR, 'official', result.buildInfo.upstreamVersion));
  }
}

main().catch((error) => {
  process.stderr.write(`plaud-linux-setup: ${error.message}\n`);
  process.exitCode = 1;
});
