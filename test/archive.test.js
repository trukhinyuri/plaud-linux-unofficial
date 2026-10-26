'use strict';

// ASAR and ZIP handling, checked against the reference implementation
// (@electron/asar) and archives written by Python's zipfile module.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { AsarArchive, integrityOf } = require('../patcher/lib/asar');
const { openZip } = require('../patcher/lib/zip');

function tmpdir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'plaud-archive-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function writeTree(root, files) {
  for (const [rel, content] of Object.entries(files)) {
    const file = path.join(root, rel);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, content);
  }
}

const SAMPLE = {
  'package.json': '{"name":"sample","version":"1.2.3","main":"main.js"}',
  'main.js': 'module.exports = 42;\n',
  'lib/deep/nested.js': 'x'.repeat(5 * 1024 * 1024 + 17), // spans several 4 MiB integrity blocks
  'lib/empty.txt': '',
  'assets/data.bin': Buffer.from(Array.from({ length: 4096 }, (_, i) => i % 256)),
  'native/addon.node': Buffer.from('not really an addon'),
};

test('reads archives written by @electron/asar, including unpacked files and integrity', async (t) => {
  const asar = await import('@electron/asar');
  const dir = tmpdir(t);
  const src = path.join(dir, 'src');
  writeTree(src, SAMPLE);
  fs.chmodSync(path.join(src, 'main.js'), 0o755);
  const out = path.join(dir, 'app.asar');
  await asar.createPackageWithOptions(src, out, { unpack: '*.node' });

  const archive = AsarArchive.open(out);
  t.after(() => archive.close());
  assert.deepEqual(archive.listFiles().sort(), Object.keys(SAMPLE).sort());
  for (const [rel, content] of Object.entries(SAMPLE)) {
    assert.deepEqual(archive.readFile(rel), Buffer.from(content), rel);
  }
  assert.equal(archive.get('native/addon.node').unpacked, true);
  assert.equal(archive.get('main.js').executable, true);
  // Our integrity computation equals the reference header's.
  const header = asar.getRawHeader(out).header;
  assert.deepEqual(integrityOf(Buffer.from(SAMPLE['lib/deep/nested.js'])), header.files.lib.files.deep.files['nested.js'].integrity);
});

test('archives written by AsarArchive are read back by @electron/asar', async (t) => {
  const asar = await import('@electron/asar');
  const dir = tmpdir(t);
  const src = path.join(dir, 'src');
  writeTree(src, SAMPLE);
  const original = path.join(dir, 'original.asar');
  await asar.createPackageWithOptions(src, original, { unpack: '*.node' });

  const archive = AsarArchive.open(original);
  archive.writeFile('main.js', Buffer.from('module.exports = 43;\n'));
  archive.writeFile('added/new.js', Buffer.from('new'), { executable: true });
  archive.writeFile('native/addon.node', archive.readFile('native/addon.node')); // repack the unpacked file
  archive.remove('lib/empty.txt');
  const extra = path.join(dir, 'extra');
  writeTree(extra, { 'a.js': 'a', 'sub/b.json': '{}', 'skip.txt': 'no' });
  archive.addDirectory(extra, 'linux-runtime', { filter: (_src, entry) => entry.isDirectory() || /\.(js|json)$/.test(entry.name) });
  const out = path.join(dir, 'patched.asar');
  archive.write(out);
  archive.close();

  assert.ok(!fs.existsSync(`${out}.unpacked`), 'no unpacked directory once every file is stored inside');
  const files = asar.listPackage(out, { isPack: false }).map((f) => f.replace(/^\//, '')).sort();
  for (const expected of ['main.js', 'added/new.js', 'native/addon.node', 'linux-runtime/a.js', 'linux-runtime/sub/b.json', 'lib/deep/nested.js']) {
    assert.ok(files.includes(expected), `${expected} listed`);
  }
  assert.ok(!files.includes('lib/empty.txt'));
  assert.ok(!files.includes('linux-runtime/skip.txt'));
  assert.equal(asar.extractFile(out, 'main.js').toString(), 'module.exports = 43;\n');
  assert.equal(asar.extractFile(out, 'native/addon.node').toString(), 'not really an addon');
  assert.equal(asar.extractFile(out, 'lib/deep/nested.js').length, SAMPLE['lib/deep/nested.js'].length);
  const header = asar.getRawHeader(out).header;
  assert.equal(header.files.added.files['new.js'].executable, true);
  assert.deepEqual(header.files['main.js'].integrity, integrityOf(Buffer.from('module.exports = 43;\n')));
});

const PY_ZIP = String.raw`
import os, stat, sys, zipfile
out = sys.argv[1]
with zipfile.ZipFile(out, "w") as z:
    def add(name, data, mode, method):
        info = zipfile.ZipInfo(name, (2026, 1, 2, 3, 4, 6))
        info.create_system = 3
        info.external_attr = mode << 16
        info.compress_type = method
        z.writestr(info, data)
    add("app/", b"", stat.S_IFDIR | 0o755, zipfile.ZIP_STORED)
    add("app/run.sh", b"#!/bin/sh\necho hi\n", stat.S_IFREG | 0o755, zipfile.ZIP_DEFLATED)
    add("app/big.txt", b"plaud " * 200000, stat.S_IFREG | 0o644, zipfile.ZIP_DEFLATED)
    add("app/empty", b"", stat.S_IFREG | 0o644, zipfile.ZIP_STORED)
    add("app/stored.bin", bytes(range(256)) * 10, stat.S_IFREG | 0o600, zipfile.ZIP_STORED)
    add("app/link", b"run.sh", stat.S_IFLNK | 0o777, zipfile.ZIP_STORED)
`;

function pythonZip(t, out) {
  try {
    execFileSync('python3', ['-c', PY_ZIP, out]);
  } catch (error) {
    t.skip(`python3 is required to write test archives: ${error.message}`);
    return false;
  }
  return true;
}

test('extracts files, modes, empty entries and symlinks with CRC verification', async (t) => {
  const dir = tmpdir(t);
  const zipPath = path.join(dir, 'test.zip');
  if (!pythonZip(t, zipPath)) return;
  const zip = openZip(zipPath);
  t.after(() => zip.close());
  const out = path.join(dir, 'out');
  const count = await zip.extractTree('app/', out);
  assert.equal(count, 5);
  assert.equal(fs.readFileSync(path.join(out, 'run.sh'), 'utf8'), '#!/bin/sh\necho hi\n');
  assert.equal(fs.statSync(path.join(out, 'run.sh')).mode & 0o777, 0o755);
  assert.equal(fs.statSync(path.join(out, 'stored.bin')).mode & 0o777, 0o600);
  assert.equal(fs.readFileSync(path.join(out, 'big.txt'), 'utf8'), 'plaud '.repeat(200000));
  assert.equal(fs.statSync(path.join(out, 'empty')).size, 0);
  assert.equal(fs.readlinkSync(path.join(out, 'link')), 'run.sh');
  assert.equal(zip.read('app/stored.bin').length, 2560);
});

test('rejects corrupted data', async (t) => {
  const dir = tmpdir(t);
  const zipPath = path.join(dir, 'test.zip');
  if (!pythonZip(t, zipPath)) return;
  const buf = fs.readFileSync(zipPath);
  // Flip one byte inside the stored payload of app/stored.bin.
  const marker = Buffer.from(Array.from({ length: 64 }, (_, i) => i));
  const at = buf.indexOf(marker);
  assert.ok(at > 0);
  buf[at + 10] ^= 0xff;
  fs.writeFileSync(zipPath, buf);
  const zip = openZip(zipPath);
  t.after(() => zip.close());
  assert.throws(() => zip.read('app/stored.bin'), /CRC-32 mismatch/);
  await assert.rejects(zip.extract('app/stored.bin', path.join(dir, 'x.bin')), /CRC-32 mismatch/);
  assert.ok(!fs.existsSync(path.join(dir, 'x.bin')));
  assert.deepEqual(fs.readdirSync(dir).filter((f) => f.includes('.part-')), [], 'partial files are removed');
});
