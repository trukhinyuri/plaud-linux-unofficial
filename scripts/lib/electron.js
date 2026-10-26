'use strict';

// Downloads stock Electron for Linux through @electron/get and unpacks it.
// Checksums are pinned in scripts/electron-checksums.json; for an unpinned
// version @electron/get verifies against the release's SHASUMS256.txt.

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { openZip } = require('../../patcher/lib/zip');
const { log, rmrf } = require('./common');

const PINNED = require('../electron-checksums.json');

/**
 * Returns the path of the verified Electron zip for linux-<arch>: `localZip`
 * when given (offline builds; it must match the pinned SHA-256), otherwise a
 * download through @electron/get (cached in its global cache).
 */
async function downloadElectron({ version, arch, localZip }) {
  const pinned = PINNED[version];
  const file = `electron-v${version}-linux-${arch}.zip`;
  if (pinned && !pinned[file]) throw new Error(`no pinned checksum for ${file}`);
  const sha256 = (p) => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
  if (localZip) {
    if (!pinned) throw new Error(`a local Electron zip needs a pinned checksum; ${version} has none`);
    const actual = sha256(localZip);
    if (actual !== pinned[file]) throw new Error(`${localZip}: SHA-256 ${actual} does not match the pinned ${pinned[file]} for ${file}`);
    log('electron', `using ${localZip} (pinned SHA-256 verified)`);
    return localZip;
  }
  const { downloadArtifact, ElectronDownloadCacheMode } = await import('@electron/get');
  const fetchZip = (cacheMode) =>
    downloadArtifact({
      version,
      platform: 'linux',
      arch,
      artifactName: 'electron',
      cacheMode,
      ...(pinned ? { checksums: pinned } : {}),
    });
  let zip = await fetchZip(ElectronDownloadCacheMode.ReadWrite);
  if (pinned) {
    // @electron/get checks downloads, not files it finds in its cache.
    if (sha256(zip) !== pinned[file]) {
      log('electron', `cached ${file} does not match the pinned SHA-256; downloading it again`);
      zip = await fetchZip(ElectronDownloadCacheMode.WriteOnly);
      if (sha256(zip) !== pinned[file]) throw new Error(`${zip}: SHA-256 does not match the pinned ${pinned[file]}`);
    }
    log('electron', `${file} ready (pinned SHA-256 verified)`);
  } else {
    log('electron', `${file} ready (verified against SHASUMS256.txt)`);
  }
  return zip;
}

/** Unpacks the Electron zip into `destDir` (replacing it). */
async function extractElectron(zipPath, destDir) {
  rmrf(destDir);
  fs.mkdirSync(destDir, { recursive: true });
  const zip = openZip(zipPath);
  try {
    const count = await zip.extractTree('', destDir);
    log('electron', `unpacked ${count} entries into ${path.relative(process.cwd(), destDir) || destDir}`);
  } finally {
    zip.close();
  }
  const binary = path.join(destDir, 'electron');
  if (!fs.existsSync(binary)) throw new Error(`Electron binary missing after extraction: ${binary}`);
  const version = fs.readFileSync(path.join(destDir, 'version'), 'utf8').trim();
  return { dir: destDir, binary, version };
}

module.exports = { downloadElectron, extractElectron };
