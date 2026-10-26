'use strict';

// electron-updater "generic" feed support: parses the latest-*.yml written by
// electron-builder, selects the macOS arm64 zip (the smallest official
// archive that carries app.asar) and verifies downloads against the feed's
// base64 SHA-512.

const crypto = require('node:crypto');
const fs = require('node:fs');
const semver = require('./semver');

class FeedError extends Error {}

function parseScalar(raw) {
  const v = raw.trim();
  if (v.startsWith("'") && v.endsWith("'") && v.length >= 2) return v.slice(1, -1).replace(/''/g, "'");
  if (v.startsWith('"') && v.endsWith('"') && v.length >= 2) return JSON.parse(v);
  if (/^-?\d+$/.test(v)) return Number(v);
  if (v === 'true') return true;
  if (v === 'false') return false;
  if (v === 'null' || v === '~' || v === '') return null;
  return v;
}

/**
 * Parses the YAML subset electron-builder emits for update feeds: top-level
 * scalars and lists of flat mappings.
 */
function parseFeedYaml(text) {
  const out = {};
  let currentList = null;
  let currentItem = null;
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  for (const [index, line] of lines.entries()) {
    if (/^\s*(#.*)?$/.test(line)) continue;
    const top = /^([A-Za-z0-9_]+):(?:\s(.*))?$/.exec(line);
    if (top) {
      const [, key, rest = ''] = top;
      if (rest.trim() === '') {
        currentList = [];
        out[key] = currentList;
      } else {
        out[key] = parseScalar(rest);
        currentList = null;
      }
      currentItem = null;
      continue;
    }
    const item = /^\s+-\s+([A-Za-z0-9_]+):(?:\s(.*))?$/.exec(line);
    if (item && currentList) {
      currentItem = { [item[1]]: parseScalar(item[2] ?? '') };
      currentList.push(currentItem);
      continue;
    }
    const cont = /^\s+([A-Za-z0-9_]+):(?:\s(.*))?$/.exec(line);
    if (cont && currentItem) {
      currentItem[cont[1]] = parseScalar(cont[2] ?? '');
      continue;
    }
    throw new FeedError(`unsupported feed syntax at line ${index + 1}: ${line}`);
  }
  return out;
}

/** Validates a parsed feed and returns the macOS zip to download. */
function selectMacZip(feed) {
  if (typeof feed.version !== 'string' && typeof feed.version !== 'number') throw new FeedError('feed has no version');
  const version = String(feed.version);
  if (!semver.parse(version)) throw new FeedError(`feed version is not semver: ${version}`);
  const files = Array.isArray(feed.files) ? feed.files : [];
  const zips = files.filter((f) => typeof f.url === 'string' && f.url.endsWith('.zip') && typeof f.sha512 === 'string');
  const pick = zips.find((f) => f.url.endsWith('-arm64-mac.zip')) ?? zips.find((f) => f.url.endsWith('-mac.zip')) ?? zips[0];
  if (!pick) throw new FeedError('feed lists no macOS zip archive');
  if (!/^[A-Za-z0-9+/]{86}==$/.test(pick.sha512)) throw new FeedError(`malformed sha512 for ${pick.url}`);
  return {
    version,
    releaseDate: feed.releaseDate ?? null,
    file: pick.url,
    sha512: pick.sha512,
    size: typeof pick.size === 'number' ? pick.size : null,
  };
}

/** Feed file name used by electron-updater for macOS on a given channel. */
function macFeedFileName(channel = 'latest') {
  return `${channel}-mac.yml`;
}

function resolveFileUrl(feedBaseUrl, file) {
  const base = feedBaseUrl.endsWith('/') ? feedBaseUrl : `${feedBaseUrl}/`;
  return new URL(encodeURI(file), base).toString();
}

/** Streams `file` through SHA-512 and returns the base64 digest. */
function sha512File(file) {
  return new Promise((resolve, reject) => {
    const hash = crypto.createHash('sha512');
    fs.createReadStream(file)
      .on('data', (d) => hash.update(d))
      .on('error', reject)
      .on('end', () => resolve(hash.digest('base64')));
  });
}

async function verifySha512(file, expected) {
  const actual = await sha512File(file);
  if (actual !== expected) {
    throw new FeedError(`SHA-512 mismatch for ${file}: expected ${expected}, got ${actual}`);
  }
  return actual;
}

module.exports = {
  parseFeedYaml,
  selectMacZip,
  macFeedFileName,
  resolveFileUrl,
  sha512File,
  verifySha512,
  FeedError,
};
