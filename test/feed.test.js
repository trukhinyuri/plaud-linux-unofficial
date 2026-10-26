'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const feed = require('../linux-runtime/feed');
const semver = require('../linux-runtime/semver');

const hash = (s) => crypto.createHash('sha512').update(s).digest('base64');

// Same layout as the electron-builder feed Plaud publishes
// (https://client-download.plaud.ai/desktop/v2/latest-mac.yml), synthetic values.
const FEED = `version: 2.0.1
files:
  - url: Plaud-2.0.1-arm64-mac.zip
    sha512: ${hash('arm64')}
    size: 134112238
  - url: Plaud-2.0.1-mac.zip
    sha512: ${hash('x64')}
    size: 146852999
  - url: Plaud-2.0.1-arm64.dmg
    sha512: ${hash('dmg')}
    size: 139152263
path: Plaud-2.0.1-arm64-mac.zip
sha512: ${hash('arm64')}
releaseDate: '2026-09-28T08:55:30.592Z'
`;

test('parses an electron-builder feed and picks the arm64 macOS zip', () => {
  const parsed = feed.parseFeedYaml(FEED);
  assert.equal(parsed.files.length, 3);
  assert.deepEqual(feed.selectMacZip(parsed), {
    version: '2.0.1',
    releaseDate: '2026-09-28T08:55:30.592Z',
    file: 'Plaud-2.0.1-arm64-mac.zip',
    sha512: hash('arm64'),
    size: 134112238,
  });
});

test('falls back to the universal macOS zip and rejects malformed feeds', () => {
  const x64Only = feed.parseFeedYaml(FEED.replace(/ {2}- url: Plaud-2\.0\.1-arm64-mac\.zip\n.*\n.*\n/, ''));
  assert.equal(feed.selectMacZip(x64Only).file, 'Plaud-2.0.1-mac.zip');
  assert.throws(() => feed.selectMacZip(feed.parseFeedYaml('version: 2.0.1\nfiles:\n  - url: a.dmg\n    sha512: x\n')), /no macOS zip/);
  assert.throws(() => feed.selectMacZip(feed.parseFeedYaml('version: banana\n')), /not semver/);
  assert.throws(() => feed.selectMacZip(feed.parseFeedYaml('version: 2.0.1\nfiles:\n  - url: a-mac.zip\n    sha512: short\n')), /malformed sha512/);
  assert.throws(() => feed.parseFeedYaml('version: 1\n  nested: [1, 2]\n'), /unsupported feed syntax/);
});

test('feed URLs follow electron-updater naming', () => {
  assert.equal(feed.macFeedFileName(), 'latest-mac.yml');
  assert.equal(feed.macFeedFileName('rc'), 'rc-mac.yml');
  assert.equal(feed.resolveFileUrl('https://h/desktop/v2', 'Plaud 2.0.1.zip'), 'https://h/desktop/v2/Plaud%202.0.1.zip');
  assert.equal(feed.resolveFileUrl('https://h/desktop/v2/', 'a.zip'), 'https://h/desktop/v2/a.zip');
});

test('verifies downloads against the base64 SHA-512', async (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'plaud-feed-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const file = path.join(dir, 'a.zip');
  fs.writeFileSync(file, 'arm64');
  assert.equal(await feed.verifySha512(file, hash('arm64')), hash('arm64'));
  await assert.rejects(feed.verifySha512(file, hash('other')), /SHA-512 mismatch/);
});

test('semantic versions compare per SemVer 2.0.0', () => {
  const ordered = ['1.0.0-alpha', '1.0.0-alpha.1', '1.0.0-alpha.beta', '1.0.0-beta', '1.0.0-beta.2', '1.0.0-beta.11', '1.0.0-rc.1', '1.0.0', '1.4.1', '1.10.0', '2.0.0'];
  for (let i = 0; i + 1 < ordered.length; i++) {
    assert.equal(semver.compare(ordered[i], ordered[i + 1]), -1, `${ordered[i]} < ${ordered[i + 1]}`);
    assert.equal(semver.compare(ordered[i + 1], ordered[i]), 1);
  }
  assert.equal(semver.compare('v1.4.1', '1.4.1+build.5'), 0);
  assert.ok(semver.isPrerelease('1.5.0-rc.1'));
  assert.ok(!semver.isPrerelease('1.5.0'));
  assert.equal(semver.parse('1.4'), null);
  assert.throws(() => semver.compare('x', '1.0.0'), TypeError);
});

test('version ranges select patch variants', () => {
  assert.ok(semver.satisfies('1.4.1', '>=1.4.1'));
  assert.ok(!semver.satisfies('1.4.0', '>=1.4.1'));
  assert.ok(semver.satisfies('1.9.9', '>=1.4.1 <2.0.0'));
  assert.ok(!semver.satisfies('2.0.0', '>=1.4.1 <2.0.0'));
  assert.ok(semver.satisfies('1.4.7', '1.4.x'));
  assert.ok(!semver.satisfies('1.5.0', '1.4.x'));
  assert.ok(semver.satisfies('1.5.0-rc.1', '>=1.5.0'), 'pre-releases match by their release triple');
  assert.ok(semver.satisfies('3.0.0', '*'));
});
