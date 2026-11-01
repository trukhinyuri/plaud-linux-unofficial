#!/usr/bin/env node
'use strict';

// Downloads the official Plaud Desktop macOS zip, verifies its SHA-512 against
// official.json (or, with --latest or --channel, against the live update feed)
// and extracts the parts the patcher needs into
// .cache/official/<version>/Resources.
//
//   node scripts/fetch-official.js [--latest | --channel latest|rc|beta|alpha] [--zip <local copy>]
//
// Prints the Resources directory on stdout.

const fs = require('node:fs');
const path = require('node:path');
const { pipeline } = require('node:stream/promises');
const { Readable } = require('node:stream');
const feed = require('../linux-runtime/feed');
const { extractOfficialResources } = require('../patcher/cli');
const { CACHE_DIR, REPO_ROOT, constants, log, parseArgs, rmrf } = require('./lib/common');

// Feed directory of each update channel, as the official app chooses it from
// its own version (resolveUpdateFeedConfig in the 1.4.1 main bundle,
// misc-DYpkuuKJ.js): x.y.z -> v2/latest-mac.yml, -rc -> rc/rc-mac.yml,
// -beta -> pre/beta-mac.yml, -alpha -> test/alpha-mac.yml.
const CHANNEL_DIRS = { latest: 'v2', rc: 'rc', beta: 'pre', alpha: 'test' };

async function resolveRelease({ latest, channel }) {
  const pinned = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'official.json'), 'utf8'));
  if (!latest && !channel) return { ...pinned, url: feed.resolveFileUrl(pinned.feed, pinned.file) };
  const name = channel ?? 'latest';
  if (!CHANNEL_DIRS[name]) throw new Error(`unknown channel "${name}" (known: ${Object.keys(CHANNEL_DIRS).join(', ')})`);
  const base = `${constants.FEED_BASE}/${CHANNEL_DIRS[name]}`;
  const feedUrl = `${base}/${feed.macFeedFileName(name)}`;
  const res = await fetch(feedUrl, { cache: 'no-store' });
  if (!res.ok) throw new Error(`GET ${feedUrl} failed with HTTP ${res.status}`);
  const info = feed.selectMacZip(feed.parseFeedYaml(await res.text()));
  return { version: info.version, file: info.file, sha512: info.sha512, size: info.size, url: feed.resolveFileUrl(base, info.file) };
}

async function download(url, dest) {
  const res = await fetch(url);
  if (!res.ok || !res.body) throw new Error(`GET ${url} failed with HTTP ${res.status}`);
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  const tmp = `${dest}.part`;
  await pipeline(Readable.fromWeb(res.body), fs.createWriteStream(tmp));
  fs.renameSync(tmp, dest);
}

/** Ensures the official release is downloaded, verified and extracted. Returns its description. */
async function fetchOfficial({ latest = false, channel, zip } = {}) {
  const release = await resolveRelease({ latest, channel });
  const dir = path.join(CACHE_DIR, 'official', release.version);
  const resourcesDir = path.join(dir, 'Resources');
  const marker = path.join(dir, 'extracted.json');
  if (fs.existsSync(marker)) {
    const done = JSON.parse(fs.readFileSync(marker, 'utf8'));
    if (done.sha512 === release.sha512 && fs.existsSync(path.join(resourcesDir, 'app.asar'))) {
      log('official', `using cached Plaud ${release.version}`);
      return { ...release, resourcesDir };
    }
  }
  const zipPath = zip ? path.resolve(zip) : path.join(CACHE_DIR, 'official', release.file);
  let verified = false;
  if (fs.existsSync(zipPath)) {
    try {
      await feed.verifySha512(zipPath, release.sha512);
      verified = true;
    } catch (error) {
      if (zip) throw error;
      log('official', 'cached download does not match; downloading again');
    }
  }
  if (!verified) {
    log('official', `downloading ${release.url}`);
    await download(release.url, zipPath);
    await feed.verifySha512(zipPath, release.sha512);
  }
  log('official', `SHA-512 verified for ${path.basename(zipPath)}`);
  rmrf(resourcesDir);
  const count = await extractOfficialResources(zipPath, resourcesDir);
  fs.writeFileSync(marker, `${JSON.stringify({ version: release.version, sha512: release.sha512 }, null, 2)}\n`);
  log('official', `extracted ${count} entries to ${path.relative(REPO_ROOT, resourcesDir)}`);
  return { ...release, resourcesDir };
}

module.exports = { fetchOfficial, resolveRelease };

if (require.main === module) {
  const args = parseArgs(process.argv.slice(2), { booleans: ['latest'] });
  fetchOfficial({
    latest: Boolean(args.latest),
    channel: typeof args.channel === 'string' ? args.channel : undefined,
    zip: typeof args.zip === 'string' ? args.zip : undefined,
  })
    .then((r) => process.stdout.write(`${r.resourcesDir}\n`))
    .catch((error) => {
      process.stderr.write(`fetch-official: ${error.message}\n`);
      process.exitCode = 1;
    });
}
