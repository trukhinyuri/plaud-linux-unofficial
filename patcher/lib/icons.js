'use strict';

// Decoders for the official icon containers:
//  - Windows ICO (tray icons; Electron's nativeImage reads ICO only on Windows)
//  - Apple ICNS (application icon; PNG-encoded members only)
// and a helper that renders a decoded image at the sizes Linux needs.

const { decodePng, encodePng, isPng, resize } = require('./png');

class IconError extends Error {}

function decodeDib32(buf, width, height) {
  const headerSize = buf.readUInt32LE(0);
  const bitCount = buf.readUInt16LE(14);
  const compression = buf.readUInt32LE(16);
  if (bitCount !== 32 || (compression !== 0 && compression !== 3)) {
    throw new IconError(`unsupported ICO bitmap (${bitCount} bpp, compression ${compression})`);
  }
  const pixelsOffset = headerSize + (compression === 3 ? 12 : 0);
  const stride = width * 4;
  const maskStride = Math.ceil(width / 32) * 4;
  const maskOffset = pixelsOffset + stride * height;
  const data = Buffer.alloc(width * height * 4);
  let anyAlpha = false;
  for (let y = 0; y < height; y++) {
    const srcRow = pixelsOffset + (height - 1 - y) * stride; // bottom-up rows
    for (let x = 0; x < width; x++) {
      const s = srcRow + x * 4;
      const d = (y * width + x) * 4;
      data[d] = buf[s + 2];
      data[d + 1] = buf[s + 1];
      data[d + 2] = buf[s];
      data[d + 3] = buf[s + 3];
      if (buf[s + 3] !== 0) anyAlpha = true;
    }
  }
  if (!anyAlpha && buf.length >= maskOffset + maskStride * height) {
    // Legacy icons without alpha: transparency comes from the AND mask.
    for (let y = 0; y < height; y++) {
      const row = maskOffset + (height - 1 - y) * maskStride;
      for (let x = 0; x < width; x++) {
        const transparent = (buf[row + (x >> 3)] >> (7 - (x & 7))) & 1;
        data[(y * width + x) * 4 + 3] = transparent ? 0 : 255;
      }
    }
  }
  return { width, height, data };
}

/** Returns every 32-bit or PNG frame of an ICO file as RGBA images. */
function decodeIco(buf) {
  if (buf.readUInt16LE(0) !== 0 || buf.readUInt16LE(2) !== 1) throw new IconError('not an ICO file');
  const count = buf.readUInt16LE(4);
  const frames = [];
  for (let i = 0; i < count; i++) {
    const e = 6 + i * 16;
    const width = buf[e] || 256;
    const height = buf[e + 1] || 256;
    const size = buf.readUInt32LE(e + 8);
    const offset = buf.readUInt32LE(e + 12);
    const body = buf.subarray(offset, offset + size);
    try {
      frames.push(isPng(body) ? decodePng(body) : decodeDib32(body, width, height));
    } catch (error) {
      if (!(error instanceof IconError)) throw error;
    }
  }
  if (frames.length === 0) throw new IconError('ICO contains no supported frames');
  return frames.sort((a, b) => b.width - a.width);
}

/** Returns the PNG members of an ICNS file as RGBA images, largest first. */
function decodeIcns(buf) {
  if (buf.toString('ascii', 0, 4) !== 'icns') throw new IconError('not an ICNS file');
  const total = buf.readUInt32BE(4);
  const frames = [];
  let pos = 8;
  while (pos + 8 <= Math.min(total, buf.length)) {
    const length = buf.readUInt32BE(pos + 4);
    if (length < 8) throw new IconError('corrupt ICNS element');
    const body = buf.subarray(pos + 8, pos + length);
    if (isPng(body)) frames.push(decodePng(body));
    pos += length;
  }
  if (frames.length === 0) throw new IconError('ICNS contains no PNG members');
  return frames.sort((a, b) => b.width - a.width);
}

/**
 * Picks the smallest frame that is at least `size` pixels (or the largest
 * available) and renders it as a `size`x`size` PNG.
 */
function renderPng(frames, size) {
  const candidates = frames.filter((f) => f.width >= size && f.width === f.height);
  const source = candidates.length > 0 ? candidates.at(-1) : frames[0];
  const image = source.width === size && source.height === size ? source : resize(source, size, size);
  return encodePng(image);
}

module.exports = { decodeIco, decodeIcns, renderPng, IconError };
