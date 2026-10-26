'use strict';

// Locates the extracted official release the tests run against: the
// directory in $PLAUD_OFFICIAL_RESOURCES, or the cache written by
// `npm run fetch-official` for the version pinned in official.json.

const fs = require('node:fs');
const path = require('node:path');

const REPO_ROOT = path.resolve(__dirname, '..', '..');
const pinned = require('../../official.json');

function officialResources() {
  const dir = process.env.PLAUD_OFFICIAL_RESOURCES || path.join(REPO_ROOT, '.cache', 'official', pinned.version, 'Resources');
  return fs.existsSync(path.join(dir, 'app.asar')) ? dir : null;
}

const SKIP_REASON = `official Plaud ${pinned.version} resources not found; run "npm run fetch-official" (or set PLAUD_OFFICIAL_RESOURCES)`;

module.exports = { REPO_ROOT, pinned, officialResources, SKIP_REASON };
