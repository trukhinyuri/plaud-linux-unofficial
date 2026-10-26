'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { encodePng, decodePng, isPng, resize } = require('../patcher/lib/png');
const { decodeIco, decodeIcns, renderPng } = require('../patcher/lib/icons');
const { AsarArchive } = require('../patcher/lib/asar');
const { officialResources, SKIP_REASON } = require('./helpers/official');

function solid(width, height, [r, g, b, a]) {
  const data = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; i++) data.set([r, g, b, a], i * 4);
  return { width, height, data };
}

test('PNG encode/decode round trip', () => {
  const image = solid(7, 5, [10, 20, 30, 40]);
  image.data[0] = 255; // a distinct first pixel exercises the filters
  const png = encodePng(image);
  assert.ok(isPng(png));
  const back = decodePng(png);
  assert.equal(back.width, 7);
  assert.equal(back.height, 5);
  assert.deepEqual(back.data, image.data);
});

test('decodes PNGs written by an independent encoder (Python zlib + filters)', (t) => {
  // A 3x2 RGB PNG with Sub/Up/Paeth filters, written by Python's zlib.
  const script = String.raw`
import struct, sys, zlib
w, h = 3, 2
rows = [bytes([1]) + bytes([10,20,30, 5,5,5, 1,2,3]), bytes([4]) + bytes([1,1,1, 2,2,2, 3,3,3])]
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
sys.stdout.buffer.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
`;
  let png;
  try {
    png = execFileSync('python3', ['-c', script]);
  } catch (error) {
    t.skip(`python3 unavailable: ${error.message}`);
    return;
  }
  const img = decodePng(png);
  // Row 0 (Sub): 10,20,30 | 15,25,35 | 16,27,38 ; row 1 (Paeth) predicted from row 0.
  assert.deepEqual([...img.data.subarray(0, 12)], [10, 20, 30, 255, 15, 25, 35, 255, 16, 27, 38, 255]);
  assert.deepEqual([...img.data.subarray(12, 16)], [11, 21, 31, 255]);
});

test('area-average resize keeps colour under transparent edges', () => {
  const image = solid(4, 4, [200, 100, 50, 255]);
  // Make the left half fully transparent black.
  for (let y = 0; y < 4; y++) for (let x = 0; x < 2; x++) image.data.set([0, 0, 0, 0], (y * 4 + x) * 4);
  const small = resize(image, 2, 2);
  assert.deepEqual([...small.data.subarray(0, 4)], [0, 0, 0, 0]);
  assert.deepEqual([...small.data.subarray(4, 8)], [200, 100, 50, 255]);
  const half = resize({ width: 2, height: 1, data: Buffer.from([200, 100, 50, 255, 0, 0, 0, 0]) }, 1, 1);
  assert.deepEqual([...half.data], [200, 100, 50, 128], 'colour is alpha-weighted, not darkened');
});

function icoWith(frames) {
  const header = Buffer.alloc(6 + frames.length * 16);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(frames.length, 4);
  let offset = header.length;
  frames.forEach((f, i) => {
    const e = 6 + i * 16;
    header[e] = f.width % 256;
    header[e + 1] = f.height % 256;
    header.writeUInt16LE(32, e + 6);
    header.writeUInt32LE(f.body.length, e + 8);
    header.writeUInt32LE(offset, e + 12);
    offset += f.body.length;
  });
  return Buffer.concat([header, ...frames.map((f) => f.body)]);
}

function dib32(width, height, pixel) {
  const info = Buffer.alloc(40);
  info.writeUInt32LE(40, 0);
  info.writeInt32LE(width, 4);
  info.writeInt32LE(height * 2, 8);
  info.writeUInt16LE(1, 12);
  info.writeUInt16LE(32, 14);
  const pixels = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; i++) pixels.set([pixel[2], pixel[1], pixel[0], pixel[3]], i * 4); // BGRA
  const mask = Buffer.alloc(Math.ceil(width / 32) * 4 * height);
  return Buffer.concat([info, pixels, mask]);
}

test('decodes ICO files with 32-bit DIB and PNG frames', () => {
  const ico = icoWith([
    { width: 16, height: 16, body: dib32(16, 16, [255, 0, 0, 128]) },
    { width: 256, height: 256, body: encodePng(solid(256, 256, [0, 0, 255, 255])) },
  ]);
  const frames = decodeIco(ico);
  assert.deepEqual(frames.map((f) => f.width), [256, 16]);
  assert.deepEqual([...frames[1].data.subarray(0, 4)], [255, 0, 0, 128]);
  const out = decodePng(renderPng(frames, 64));
  assert.equal(out.width, 64);
  assert.deepEqual([...out.data.subarray(0, 4)], [0, 0, 255, 255], 'downscaled from the 256 px frame');
  assert.equal(decodePng(renderPng(frames, 16)).width, 16);
});

test('decodes the PNG members of ICNS files', () => {
  const member = (type, png) => {
    const head = Buffer.alloc(8);
    head.write(type, 0, 'ascii');
    head.writeUInt32BE(png.length + 8, 4);
    return Buffer.concat([head, png]);
  };
  const body = Buffer.concat([member('ic07', encodePng(solid(128, 128, [1, 2, 3, 255]))), member('ic09', encodePng(solid(512, 512, [4, 5, 6, 255])))]);
  const head = Buffer.alloc(8);
  head.write('icns', 0, 'ascii');
  head.writeUInt32BE(body.length + 8, 4);
  const frames = decodeIcns(Buffer.concat([head, body]));
  assert.deepEqual(frames.map((f) => f.width), [512, 128]);
  assert.deepEqual([...decodePng(renderPng(frames, 128)).data.subarray(0, 4)], [1, 2, 3, 255], 'exact-size member preferred');
});

const resources = officialResources();
test('renders the official application and tray icons', { skip: resources ? false : SKIP_REASON }, () => {
  const app = decodeIcns(fs.readFileSync(path.join(resources, 'icon.icns')));
  assert.ok(app[0].width >= 512);
  for (const size of [16, 24, 32, 48, 64, 128, 256, 512]) assert.equal(decodePng(renderPng(app, size)).width, size);
  const archive = AsarArchive.open(path.join(resources, 'app.asar'));
  try {
    for (const name of ['Icon-off', 'Icon-on', 'Icon-ppc', 'Icon-update']) {
      const frames = decodeIco(archive.readFile(`resources/tray/${name}.ico`));
      const tray = decodePng(renderPng(frames, 64));
      const opaque = tray.data.filter((_, i) => i % 4 === 3 && tray.data[i] > 0).length;
      assert.ok(opaque > 64 * 64 * 0.05, `${name} has visible pixels`);
      assert.ok(opaque < 64 * 64, `${name} keeps its transparency`);
    }
  } finally {
    archive.close();
  }
});
