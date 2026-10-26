'use strict';

// A runtime ID fingerprints everything a staged (re-patched) release depends
// on besides the official code itself: the patcher, the Linux runtime, the
// native API of the bundled addons and the Electron version. A staged
// release is only used by a build with the same runtime ID.

const crypto = require('node:crypto');
const path = require('node:path');
const fs = require('./fs');

const GENERATED = new Set(['patch-manifest.json']);

function listFiles(dir, rel = '') {
  const out = [];
  for (const entry of fs.readdirSync(path.join(dir, rel), { withFileTypes: true })) {
    const relPath = rel ? `${rel}/${entry.name}` : entry.name;
    if (entry.isDirectory()) {
      if (entry.name === 'node_modules' || entry.name === 'test') continue;
      out.push(...listFiles(dir, relPath));
    } else if (entry.isFile() && /\.(js|json)$/.test(entry.name) && !GENERATED.has(entry.name)) {
      out.push(relPath);
    }
  }
  return out.sort();
}

function computeRuntimeId({ patcherDir, runtimeDir, nativeApi, electronVersion }) {
  const hash = crypto.createHash('sha256');
  for (const [label, dir] of [['patcher', patcherDir], ['runtime', runtimeDir]]) {
    for (const file of listFiles(dir)) {
      hash.update(`${label}/${file}\0`);
      hash.update(fs.readFileSync(path.join(dir, file)));
      hash.update('\0');
    }
  }
  hash.update(`native-api\0${JSON.stringify(nativeApi)}\0electron\0${electronVersion}`);
  return hash.digest('hex').slice(0, 32);
}

module.exports = { computeRuntimeId, listFiles };
