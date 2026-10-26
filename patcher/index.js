'use strict';

// Turns the official macOS app.asar into a Linux app.asar:
//  1. applies the anchored source patches from patches.js,
//  2. renders Linux tray icons from the official ICO files,
//  3. removes macOS-only native prebuilds and folds app.asar.unpacked back in,
//  4. injects the Linux runtime (linux-runtime/) and a patch manifest,
//  5. points package.json at the Linux loader.
// Used by the build (scripts/build.js) and, at run time, by the in-app
// updater (patcher/cli.js stage) — so it depends on Node built-ins only.

const crypto = require('node:crypto');
const path = require('node:path');
const fs = require('./lib/fs');
const { AsarArchive } = require('./lib/asar');
const { decodeIco, renderPng } = require('./lib/icons');
const semver = require('../linux-runtime/semver');
const surfaceLib = require('./lib/api-surface');
const constants = require('../linux-runtime/constants');
const PATCHES = require('./patches');

class PatchError extends Error {
  constructor(message, details = {}) {
    super(message);
    this.name = 'PatchError';
    Object.assign(this, details);
  }
}

const LOADER = 'linux-runtime/loader.js';
const MANIFEST = 'linux-runtime/patch-manifest.json';
const PROTOCOL_CLIENT_PREBUILDS = 'node_modules/electron-app-universal-protocol-client/prebuilds';
const TRAY_ICONS = { off: 'Icon-off.ico', on: 'Icon-on.ico', ppc: 'Icon-ppc.ico', update: 'Icon-update.ico' };
const TRAY_ICON_SIZE = 64;

const sha256 = (data) => crypto.createHash('sha256').update(data).digest('hex');

function globToRegExp(glob) {
  const escaped = glob.split('*').map((s) => s.replace(/[.+?^${}()|[\]\\]/g, '\\$&'));
  return new RegExp(`^${escaped.join('[^/]*')}$`);
}

function countOccurrences(haystack, needle) {
  let count = 0;
  for (let i = haystack.indexOf(needle); i !== -1; i = haystack.indexOf(needle, i + needle.length)) count++;
  return count;
}

/** Selects, for each patch id, the single variant that applies to `version`. */
function selectPatches(version, patches = PATCHES) {
  const ids = [...new Set(patches.map((p) => p.id))];
  return ids.map((id) => {
    const variants = patches.filter((p) => p.id === id && semver.satisfies(version, p.versions));
    if (variants.length !== 1) {
      throw new PatchError(
        `patch "${id}" has ${variants.length} variants for Plaud ${version}; this release is not supported by this build`,
        { code: 'UNSUPPORTED_VERSION', patchId: id },
      );
    }
    return variants[0];
  });
}

/**
 * Applies the patches to the archive in place. Returns per-patch records for
 * the manifest. Throws PatchError when an anchor is missing or ambiguous.
 */
function applyPatches(archive, version, patches = PATCHES) {
  const files = archive.listFiles();
  const cache = new Map();
  const read = (file) => {
    if (!cache.has(file)) cache.set(file, archive.readFile(file).toString('utf8'));
    return cache.get(file);
  };
  const originals = new Map();
  const applied = [];
  for (const patch of selectPatches(version, patches)) {
    const re = globToRegExp(patch.file);
    const matching = files.filter((f) => re.test(f));
    const hits = matching
      .map((file) => ({ file, count: countOccurrences(read(file), patch.anchor) }))
      .filter((h) => h.count > 0);
    const total = hits.reduce((n, h) => n + h.count, 0);
    if (total !== 1) {
      const alreadyPatched = matching.some((f) => read(f).includes(patch.replacement));
      const where = hits.map((h) => `${h.file} (${h.count}x)`).join(', ');
      throw new PatchError(
        total === 0
          ? `patch "${patch.id}": anchor not found in ${patch.file}${alreadyPatched ? ' (the replacement is already present: input is already patched)' : ''}`
          : `patch "${patch.id}": anchor is ambiguous, found ${total} times: ${where}`,
        { code: total === 0 ? 'ANCHOR_NOT_FOUND' : 'ANCHOR_AMBIGUOUS', patchId: patch.id, anchor: patch.anchor },
      );
    }
    const { file } = hits[0];
    if (!originals.has(file)) originals.set(file, read(file));
    cache.set(file, read(file).replace(patch.anchor, () => patch.replacement));
    applied.push({ id: patch.id, file });
  }
  const records = [];
  for (const { id, file } of applied) {
    records.push({ id, file, sha256Before: sha256(originals.get(file)), sha256After: sha256(cache.get(file)) });
  }
  for (const file of originals.keys()) archive.writeFile(file, Buffer.from(cache.get(file), 'utf8'));
  return records;
}

/** Renders resources/tray/linux/Icon-*.png from the official ICO tray icons. */
function addTrayIcons(archive) {
  for (const [name, ico] of Object.entries(TRAY_ICONS)) {
    const source = `resources/tray/${ico}`;
    if (!archive.has(source)) throw new PatchError(`tray icon missing from the official archive: ${source}`, { code: 'ASSET_MISSING' });
    const frames = decodeIco(archive.readFile(source));
    archive.writeFile(`resources/tray/linux/Icon-${name}.png`, renderPng(frames, TRAY_ICON_SIZE));
  }
}

/** Drops macOS-only prebuilds and stores every remaining file inside the archive. */
function normalizeNativeContent(archive) {
  archive.remove(PROTOCOL_CLIENT_PREBUILDS);
  for (const file of archive.listFiles()) {
    const node = archive.get(file);
    if (node.unpacked) archive.writeFile(file, archive.readFile(file), { executable: node.executable });
    if (/\.(node|dylib)$/.test(file)) {
      throw new PatchError(`unexpected native binary in the official archive: ${file}`, { code: 'UNEXPECTED_NATIVE' });
    }
  }
}

function injectRuntime(archive, runtimeDir) {
  archive.remove('linux-runtime');
  archive.addDirectory(runtimeDir, 'linux-runtime', {
    filter: (src, entry) => entry.isDirectory() || (/\.(js|json)$/.test(entry.name) && entry.name !== 'patch-manifest.json'),
  });
  if (!archive.has(LOADER)) throw new PatchError(`runtime directory lacks ${LOADER}`, { code: 'RUNTIME_MISSING' });
}

/** Reads the API surface inputs from an extracted official Resources directory. */
function readSurfaceInputs(archive, resourcesDir) {
  const mainSources = new Map();
  for (const file of archive.listFiles()) {
    if (file.startsWith('out-global-online/main/') && file.endsWith('.js')) {
      mainSources.set(file, archive.readFile(file).toString('utf8'));
    }
  }
  const nativeBinaries = new Map();
  const nodeFiles = archive.listFiles().filter((f) => f.endsWith('.node'));
  for (const entry of fs.readdirSync(resourcesDir)) {
    if (entry.endsWith('.node')) {
      nativeBinaries.set(entry, fs.readFileSync(path.join(resourcesDir, entry)));
      nodeFiles.push(entry);
    }
  }
  if (nativeBinaries.size === 0) {
    throw new PatchError(`no official native addons found in ${resourcesDir}`, { code: 'ASSET_MISSING' });
  }
  return { mainSources, nativeBinaries, nodeFiles };
}

/**
 * Patches an extracted official Resources directory into `outAsar`.
 *
 * @param {object} options
 * @param {string} options.resourcesDir  directory holding app.asar (+ app.asar.unpacked, *.node)
 * @param {string} options.outAsar       output archive path
 * @param {string} options.runtimeDir    linux-runtime sources to inject
 * @param {string} options.runtimeId     fingerprint of patcher + runtime + addons
 * @param {object} [options.nativeApi]   exports of the Linux addons (native-api.json)
 * @param {object} [options.baseline]    API surface of the release this build was verified against
 * @param {boolean} [options.computeBaseline] return the surface of this release instead of checking it
 */
function patchResources({ resourcesDir, outAsar, runtimeDir, runtimeId, nativeApi, baseline, computeBaseline = false }) {
  const archive = AsarArchive.open(path.join(resourcesDir, 'app.asar'));
  try {
    const pkg = JSON.parse(archive.readFile('package.json').toString('utf8'));
    const version = String(pkg.version);
    if (!semver.parse(version)) throw new PatchError(`official package.json has a non-semver version: ${version}`);
    if (pkg.main === LOADER || archive.has(MANIFEST)) {
      throw new PatchError('input archive is already patched', { code: 'ALREADY_PATCHED' });
    }

    const surface = surfaceLib.computeSurface(readSurfaceInputs(archive, resourcesDir));
    let surfaceCheck = null;
    if (!computeBaseline) {
      if (!nativeApi || !baseline) throw new PatchError('nativeApi and baseline are required to check the API surface');
      surfaceCheck = surfaceLib.checkSurface({ surface, baseline, nativeApi });
      if (!surfaceCheck.ok) {
        throw new PatchError(`Plaud ${version} needs native functionality this build lacks:\n- ${surfaceCheck.problems.join('\n- ')}`, {
          code: 'API_SURFACE',
          problems: surfaceCheck.problems,
        });
      }
    }

    const patches = applyPatches(archive, version);
    addTrayIcons(archive);
    normalizeNativeContent(archive);
    injectRuntime(archive, runtimeDir);

    const manifest = {
      schema: 1,
      upstreamVersion: version,
      upstreamMain: pkg.main,
      runtimeId,
      electron: constants.ELECTRON_VERSION,
      patches,
    };
    archive.writeFile(MANIFEST, Buffer.from(`${JSON.stringify(manifest, null, 2)}\n`));

    const newPkg = {
      ...pkg,
      main: LOADER,
      desktopName: `${constants.APP_ID}.desktop`,
      plaudLinux: { upstreamMain: pkg.main, upstreamVersion: version, runtimeId },
    };
    archive.writeFile('package.json', Buffer.from(`${JSON.stringify(newPkg, null, 2)}\n`));
    archive.write(outAsar);
    return { version, manifest, surface };
  } finally {
    archive.close();
  }
}

module.exports = {
  PatchError,
  PATCHES,
  selectPatches,
  applyPatches,
  patchResources,
  globToRegExp,
  countOccurrences,
  LOADER,
  MANIFEST,
};
