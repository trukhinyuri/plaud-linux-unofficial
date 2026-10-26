// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Screen-capture statics of recorder_linux.node: argument validation parity
// with the official addon, permission/OCR/window contracts, and live captures
// (skipped when no capture backend is usable; CI runs them in an X11 session
// on Xvfb with a window manager, see scripts/ci/x11-desktop.sh).
//
//   node --test test/screen.test.mjs
//   RECORDER_ADDON=/path/to/addon.node node --test test/screen.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { inflateSync } from 'node:zlib';
import fs from 'node:fs';
import path from 'node:path';

const require = createRequire(import.meta.url);
const addonPath =
  process.env.RECORDER_ADDON || fileURLToPath(new URL('../build/Release/recorder_linux.node', import.meta.url));
const { Recorder } = require(addonPath);

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const LEVELS = new Set(['info', 'warn', 'error']);

function collector() {
  const logs = [];
  const cb = function (entry) {
    logs.push({ self: this, entry });
  };
  return { cb, logs, messages: () => logs.map((l) => l.entry.message) };
}

function withEnv(vars, fn) {
  const saved = {};
  for (const [k, v] of Object.entries(vars)) {
    saved[k] = process.env[k];
    if (v === undefined) delete process.env[k];
    else process.env[k] = v;
  }
  try {
    return fn();
  } finally {
    for (const [k, v] of Object.entries(saved)) {
      if (v === undefined) delete process.env[k];
      else process.env[k] = v;
    }
  }
}

function pngSize(buf) {
  assert.ok(Buffer.isBuffer(buf), 'expected a Buffer');
  assert.ok(buf.subarray(0, 8).equals(PNG_SIGNATURE), 'expected a PNG signature');
  return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) };
}

// Decoder for the 8-bit RGB, non-interlaced PNGs the addon produces.
function decodePng(buf) {
  pngSize(buf);
  let off = 8;
  let width = 0;
  let height = 0;
  const idat = [];
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('latin1', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      assert.equal(data[8], 8, 'bit depth');
      assert.equal(data[9], 2, 'color type RGB');
      assert.equal(data[12], 0, 'non-interlaced');
    } else if (type === 'IDAT') {
      idat.push(data);
    } else if (type === 'IEND') {
      break;
    }
    off += 12 + len;
  }
  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * 3;
  assert.equal(raw.length, (stride + 1) * height, 'decoded size');
  const px = Buffer.alloc(stride * height);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const src = y * (stride + 1) + 1;
    const dst = y * stride;
    for (let x = 0; x < stride; x++) {
      const a = x >= 3 ? px[dst + x - 3] : 0;
      const b = y > 0 ? px[dst - stride + x] : 0;
      const c = x >= 3 && y > 0 ? px[dst - stride + x - 3] : 0;
      let v = raw[src + x];
      if (filter === 1) v += a;
      else if (filter === 2) v += b;
      else if (filter === 3) v += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a);
        const pb = Math.abs(p - b);
        const pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      } else if (filter !== 0) throw new Error(`bad PNG filter ${filter}`);
      px[dst + x] = v & 255;
    }
  }
  return { width, height, px };
}

function distinctColors(img, limit = 16) {
  const seen = new Set();
  const step = Math.max(1, Math.floor((img.width * img.height) / 4096));
  for (let i = 0; i < img.width * img.height && seen.size < limit; i += step) {
    seen.add(img.px.readUIntBE(i * 3, 3));
  }
  return seen.size;
}

function assertThrows(fn, ctor, message) {
  assert.throws(fn, (e) => {
    assert.equal(e.constructor, ctor, `expected ${ctor.name}, got ${e.constructor.name}: ${e.message}`);
    assert.equal(e.message, message);
    return true;
  });
}

// ---------------------------------------------------------------- validation

test('captureScreenImage rejects invalid first/second arguments', () => {
  assertThrows(() => Recorder.captureScreenImage(5), TypeError, 'First argument must be an options object or a callback function.');
  assertThrows(() => Recorder.captureScreenImage('main'), TypeError, 'First argument must be an options object or a callback function.');
  assertThrows(() => Recorder.captureScreenImage(true, () => {}), TypeError, 'First argument must be an options object or a callback function.');
  assertThrows(() => Recorder.captureScreenImage({}, 5), TypeError, 'Second argument must be a callback function.');
  assertThrows(() => Recorder.captureScreenImage([{}], 'cb'), TypeError, 'Second argument must be a callback function.');
});

test('captureScreenImage validates options arrays and request objects', () => {
  const cases = [
    [[], TypeError, 'options array must be non-empty.'],
    [[5], TypeError, 'options array elements must be objects.'],
    [{ requests: [] }, TypeError, 'requests array must be non-empty.'],
    [{ requests: [1] }, TypeError, 'requests array elements must be objects.'],
    [{ requests: [{ mode: 'nope' }] }, TypeError, "mode must be 'main', 'cursor', or 'window'."],
    [{ displayId: [] }, TypeError, 'displayId array must be non-empty.'],
    [{ displayId: ['1'] }, TypeError, 'displayId array elements must be numbers.'],
    [{ displayId: [-1] }, RangeError, 'displayId must be >= 0.'],
    [{ displayId: -0.5 }, RangeError, 'displayId must be >= 0.'],
    [{ displayIndex: [] }, TypeError, 'displayIndex array must be non-empty.'],
    [{ displayIndex: [null] }, TypeError, 'displayIndex array elements must be numbers.'],
    [{ displayIndex: [-1] }, RangeError, 'displayIndex must be >= 0.'],
    [{ displayIndex: -3 }, RangeError, 'displayIndex must be >= 0.'],
    [{ windowId: [] }, TypeError, 'windowId array must be non-empty.'],
    [{ windowId: [{}] }, TypeError, 'windowId array elements must be numbers.'],
    [{ windowId: [0] }, RangeError, 'windowId must be > 0.'],
    [{ windowId: -7 }, RangeError, 'windowId must be > 0.'],
    [{ rect: [] }, TypeError, 'rect array must be non-empty.'],
    [{ rect: [1] }, TypeError, 'rect array elements must be objects.'],
    [{ rect: { x: 0, y: 0, width: 10 } }, TypeError, 'rect must have numeric {x, y, width, height}.'],
    [{ rect: [{ x: '0', y: 0, width: 10, height: 10 }] }, TypeError, 'rect must have numeric {x, y, width, height}.'],
    [{ rect: { x: 0, y: 0, width: 0, height: 10 } }, RangeError, 'rect width/height must be > 0.'],
    [{ rect: [{ x: 0, y: 0, width: 5, height: -1 }] }, RangeError, 'rect width/height must be > 0.'],
    [{ mode: 'screen' }, TypeError, "mode must be 'main', 'cursor', or 'window'."],
    [[{ displayIndex: 0 }, { windowId: 0 }], RangeError, 'windowId must be > 0.'],
  ];
  for (const [options, ctor, message] of cases) {
    assertThrows(() => Recorder.captureScreenImage(options), ctor, message);
  }
});

test('request keys follow the official precedence and skip wrong types', () => {
  // displayId wins over a broken mode; a non-number displayId is skipped.
  assertThrows(() => Recorder.captureScreenImage({ displayId: 'x', mode: 'bad' }), TypeError, "mode must be 'main', 'cursor', or 'window'.");
  assertThrows(() => Recorder.captureScreenImage({ displayId: -1, mode: 'bad' }), RangeError, 'displayId must be >= 0.');
  assertThrows(() => Recorder.captureScreenImage({ windowId: 0, rect: { x: 0 } }), RangeError, 'windowId must be > 0.');
  // A key that is present but not an array falls through to request parsing.
  assertThrows(() => Recorder.captureScreenImage({ requests: 'x', rect: { x: 0, y: 0, width: 1, height: 0 } }), RangeError, 'rect width/height must be > 0.');
});

test('a NaN rect passes validation and fails at capture time with logs', () => {
  const { cb, logs, messages } = collector();
  assertThrows(() => Recorder.captureScreenImage({ rect: { x: 0, y: 0, width: NaN, height: 10 } }, cb), Error, 'Failed to capture screen image.');
  assert.ok(messages().includes('CaptureScreenRectPng: non-finite rect values.'));
  for (const { self, entry } of logs) {
    assert.equal(self, undefined, 'callback is invoked with this === undefined');
    assert.deepEqual(Object.keys(entry), ['level', 'message']);
    assert.ok(LEVELS.has(entry.level), entry.level);
  }
});

test('batch failures name the 0-based batch index and prefix logs', () => {
  const { cb, messages } = collector();
  assertThrows(
    () => Recorder.captureScreenImage({ rect: [{ x: 0, y: 0, width: Infinity, height: 1 }, { x: 0, y: 0, width: 1, height: 1 }] }, cb),
    Error,
    'Failed to capture screen image (batch index 0).',
  );
  assert.ok(messages().length > 0);
  for (const m of messages()) assert.ok(m.startsWith('[0] '), m);
});

test('an exception thrown by the capture log callback is rethrown after the capture', () => {
  const boom = new Error('boom');
  let calls = 0;
  assert.throws(
    () => Recorder.captureScreenImage({ rect: { x: 0, y: 0, width: NaN, height: 1 } }, () => {
      calls++;
      throw boom;
    }),
    (e) => e === boom,
  );
  assert.equal(calls, 1, 'logging stops after the first exception');
  assert.throws(
    () => Recorder.captureScreenImage({ rect: { x: 0, y: 0, width: NaN, height: 1 } }, () => {
      throw 42;
    }),
    (e) => e === 42,
  );
});

test('permission statics return the official types and log synchronously', () => {
  const { cb, logs } = collector();
  const status = Recorder.checkScreenCapturePermission(cb);
  assert.ok(status === 0 || status === 1, `status ${status}`);
  assert.deepEqual(logs[0].entry, { level: 'info', message: 'Starting screen capture permission check' });
  assert.equal(logs[0].self, undefined);

  assert.equal(Recorder.checkScreenCapturePermission(), status);
  assert.equal(Recorder.checkScreenCapturePermission('ignored non-function'), status);
  assert.equal(Recorder.checkScreenCapturePermissionFresh(), status);
  assert.equal(Recorder.checkScreenCapturePermissionFresh(() => {}), status);
  assert.equal(Recorder.checkScreenCapturePermissionFresh('/nonexistent/probe', () => {}), status);
  assert.equal(Recorder.requestScreenCapturePermission(), status === 0);
  assert.equal(Recorder.requestScreenCapturePermission(() => {}), status === 0);
  // Callback exceptions do not escape the permission statics.
  assert.equal(Recorder.checkScreenCapturePermission(() => {
    throw new Error('ignored');
  }), status);

  const message = 'checkScreenCapturePermissionFresh expects (callback) or (probePath, callback)';
  assertThrows(() => Recorder.checkScreenCapturePermissionFresh(1), TypeError, message);
  assertThrows(() => Recorder.checkScreenCapturePermissionFresh(undefined), TypeError, message);
  assertThrows(() => Recorder.checkScreenCapturePermissionFresh('/probe'), TypeError, message);
  assertThrows(() => Recorder.checkScreenCapturePermissionFresh(() => {}, '/probe'), TypeError, message);
  assertThrows(() => Recorder.checkScreenCapturePermissionFresh('/probe', () => {}, 3), TypeError, message);
});

test('getWindowAtCursor accepts only an optional callback', () => {
  const message = 'getWindowAtCursor expects an optional callback';
  assertThrows(() => Recorder.getWindowAtCursor(undefined), TypeError, message);
  assertThrows(() => Recorder.getWindowAtCursor(1), TypeError, message);
  assertThrows(() => Recorder.getWindowAtCursor(() => {}, 2), TypeError, message);
});

test('extractTextFromImage validates arguments and reports missing OCR', () => {
  assertThrows(() => Recorder.extractTextFromImage(), TypeError, 'First argument must be a PNG Buffer.');
  assertThrows(() => Recorder.extractTextFromImage(123), TypeError, 'First argument must be a PNG Buffer.');
  assertThrows(() => Recorder.extractTextFromImage('png'), TypeError, 'First argument must be a PNG Buffer.');
  assertThrows(() => Recorder.extractTextFromImage(Buffer.alloc(8), 5), TypeError, 'Second argument must be a callback function.');
  assertThrows(() => Recorder.extractTextFromImage(Buffer.alloc(8), null), Error, 'Failed to extract text from image (Vision OCR).');
  const { cb, logs } = collector();
  assertThrows(() => Recorder.extractTextFromImage(PNG_SIGNATURE, cb), Error, 'Failed to extract text from image (Vision OCR).');
  assert.deepEqual(logs.map((l) => l.entry), [{ level: 'warn', message: 'OCR is not implemented on Linux in this build.' }]);
});

// ---------------------------------------------------------------- live capture

const liveDisabled = process.env.PLAUD_SCREEN_TESTS_LIVE === '0';
const captureAvailable = !liveDisabled && Recorder.checkScreenCapturePermission() === 0;
const live = { skip: captureAvailable ? false : 'no usable screen-capture backend (or PLAUD_SCREEN_TESTS_LIVE=0)' };

// Desktop layout as the addon reports it in its setup log line. A rect far
// outside every display makes the addon resolve its setup and fail without
// capturing pixels.
function layout(coords, backend) {
  const { cb, messages } = collector();
  withEnv({ PLAUD_SCREEN_COORDS: coords, PLAUD_SCREEN_BACKEND: backend }, () => {
    assert.throws(() => Recorder.captureScreenImage({ rect: { x: -1e8, y: -1e8, width: 1, height: 1 } }, cb));
  });
  const line = messages().find((m) => m.startsWith('Screen capture setup:'));
  assert.ok(line, `no setup line in ${JSON.stringify(messages())}`);
  assert.ok(messages().includes('CaptureScreenRectPng: requested rect is outside the virtual screen.'));
  const re = /(\S+) (\d+)x(\d+) logical=\((\S+), (\S+), (\S+), (\S+)\)(?: x11=\((-?\d+), (-?\d+), (\d+), (\d+)\) xid=(\d+))?/g;
  const displays = [];
  for (const m of line.slice(line.indexOf('displays=[') + 'displays=['.length).matchAll(re)) {
    const [pw, ph, lx, ly, lw, lh] = [m[2], m[3], m[4], m[5], m[6], m[7]].map(Number);
    const s = pw / lw;
    displays.push({
      name: m[1],
      pw,
      ph,
      lx,
      ly,
      lw,
      lh,
      wayland: { x: Math.round(lx * s), y: Math.round(ly * s), width: pw, height: ph },
      x11: m[8] === undefined ? null : { x: +m[8], y: +m[9], width: +m[10], height: +m[11] },
      xid: m[12] === undefined ? null : +m[12],
    });
  }
  assert.ok(displays.length > 0, line);
  const space = /coords=(\w+)/.exec(line)[1];
  return { line, displays, space };
}

function hyprMonitors() {
  try {
    return JSON.parse(execFileSync('hyprctl', ['-j', 'monitors'], { encoding: 'utf8', timeout: 3000 })).filter((m) => !m.disabled);
  } catch {
    return null;
  }
}

function capture(options, env = {}) {
  const { cb, logs, messages } = collector();
  const result = withEnv(env, () => Recorder.captureScreenImage(options, cb));
  for (const { entry } of logs) assert.ok(LEVELS.has(entry.level), entry.level);
  return { result, messages };
}

// Expected composite of rect `t` (space coordinates) from whole-display
// images: clip to the bounding box, black where no display is.
function expectedComposite(t, rects, images) {
  const x0 = Math.min(...rects.map((r) => r.x));
  const y0 = Math.min(...rects.map((r) => r.y));
  const x1 = Math.max(...rects.map((r) => r.x + r.width));
  const y1 = Math.max(...rects.map((r) => r.y + r.height));
  const left = Math.max(Math.floor(t.x), x0);
  const top = Math.max(Math.floor(t.y), y0);
  const right = Math.min(Math.ceil(t.x + t.width), x1);
  const bottom = Math.min(Math.ceil(t.y + t.height), y1);
  const width = right - left;
  const height = bottom - top;
  const px = Buffer.alloc(width * height * 3);
  for (let i = 0; i < rects.length; i++) {
    const r = rects[i];
    const img = images[i];
    for (let y = Math.max(top, r.y); y < Math.min(bottom, r.y + r.height); y++) {
      const from = ((y - r.y) * img.width + (Math.max(left, r.x) - r.x)) * 3;
      const to = Math.min(right, r.x + r.width);
      const count = (to - Math.max(left, r.x)) * 3;
      if (count > 0) img.px.copy(px, ((y - top) * width + (Math.max(left, r.x) - left)) * 3, from, from + count);
    }
  }
  return { width, height, px };
}

function assertComposites(rects, env) {
  const probes = [];
  for (const r of rects) {
    probes.push({ x: r.x - 40, y: r.y - 30, width: 160, height: 120 });
    probes.push({ x: r.x + 17, y: r.y + 23, width: 301, height: 157 });
    probes.push({ x: r.x + r.width - 100.5, y: r.y + r.height - 60.25, width: 250, height: 90.5 });
  }
  const { result } = capture({ rect: [...rects, ...probes] }, env);
  assert.ok(Array.isArray(result));
  assert.equal(result.length, rects.length + probes.length);
  const images = result.slice(0, rects.length).map(decodePng);
  images.forEach((img, i) => {
    assert.deepEqual([img.width, img.height], [rects[i].width, rects[i].height]);
    assert.ok(distinctColors(img) > 1, 'display image is not uniform');
  });
  probes.forEach((p, i) => {
    const got = decodePng(result[rects.length + i]);
    const want = expectedComposite(p, rects, images);
    assert.deepEqual([got.width, got.height], [want.width, want.height], `probe ${JSON.stringify(p)}`);
    assert.ok(got.px.equals(want.px), `probe ${JSON.stringify(p)} pixels differ from the display images`);
  });
}

test('wayland coordinates: per-display rects and composites are pixel-exact', live, () => {
  const { displays } = layout('wayland');
  const monitors = hyprMonitors();
  if (monitors) {
    for (const d of displays) {
      const m = monitors.find((x) => x.name === d.name);
      assert.ok(m, `hyprctl lists ${d.name}`);
      const rotated = m.transform % 2 === 1;
      assert.deepEqual([d.pw, d.ph], rotated ? [m.height, m.width] : [m.width, m.height]);
      assert.deepEqual([d.lx, d.ly], [m.x, m.y]);
    }
  }
  assertComposites(displays.map((d) => d.wayland), { PLAUD_SCREEN_COORDS: 'wayland' });
});

test('wayland coordinates: Electron physicalBounds rects return whole displays', live, () => {
  const { displays } = layout('wayland');
  // Plaud computes origin = round(dipToScreenPoint(bounds.origin + 2) - 2 * scaleFactor);
  // dipToScreenPoint is the identity on Wayland.
  const rects = displays.map((d) => {
    const sf = d.pw / d.lw;
    return {
      x: Math.round(d.lx + 2 - 2 * sf),
      y: Math.round(d.ly + 2 - 2 * sf),
      width: Math.round(d.lw * sf),
      height: Math.round(d.lh * sf),
    };
  });
  const { result, messages } = capture({ rect: rects }, { PLAUD_SCREEN_COORDS: 'wayland' });
  const list = Array.isArray(result) ? result : [result];
  assert.equal(list.length, rects.length);
  list.forEach((buf, i) => assert.deepEqual(pngSize(buf), { width: rects[i].width, height: rects[i].height }));
  assert.equal(messages().filter((m) => m.includes('capturing the whole display')).length, rects.length);
});

test('x11 coordinates: XWayland/X11 rects and composites', live, (t) => {
  const { displays } = layout('x11');
  if (displays.some((d) => !d.x11)) return t.skip('no X11 geometry for every display');
  const rects = displays.map((d) => d.x11);
  if (displays.every((d) => d.x11.width === d.pw && d.x11.height === d.ph)) {
    assertComposites(rects, { PLAUD_SCREEN_COORDS: 'x11' });
  } else {
    const { result } = capture({ rect: rects }, { PLAUD_SCREEN_COORDS: 'x11' });
    const list = Array.isArray(result) ? result : [result];
    list.forEach((buf, i) => assert.deepEqual(pngSize(buf), { width: rects[i].width, height: rects[i].height }));
  }
  // Electron's display.id under X11 is the RandR output XID.
  const ids = displays.map((d) => d.xid);
  const { result } = capture({ displayId: ids }, { PLAUD_SCREEN_COORDS: 'x11' });
  const list = Array.isArray(result) ? result : [result];
  list.forEach((buf, i) => assert.deepEqual(pngSize(buf), { width: displays[i].pw, height: displays[i].ph }));
});

test('a rect naming a display in the other coordinate space selects that display', live, (t) => {
  const { displays } = layout('x11');
  if (displays.some((d) => !d.x11)) return t.skip('no X11 geometry for every display');
  const electronWayland = displays.map((d) => {
    const sf = d.pw / d.lw;
    return { x: Math.round(d.lx + 2 - 2 * sf), y: Math.round(d.ly + 2 - 2 * sf), width: d.pw, height: d.ph };
  });
  for (const [coords, rects] of [['wayland', displays.map((d) => d.x11)], ['x11', electronWayland]]) {
    const { result, messages } = capture({ rect: rects }, { PLAUD_SCREEN_COORDS: coords });
    const list = Array.isArray(result) ? result : [result];
    list.forEach((buf, i) => assert.deepEqual(pngSize(buf), { width: rects[i].width, height: rects[i].height }));
    assert.equal(messages().filter((m) => m.includes('capturing the whole display')).length, rects.length, coords);
  }
});

test('rects use floor/ceil bounds and clip to the virtual screen', live, () => {
  const { displays } = layout('wayland');
  const d = displays[0].wayland;
  const env = { PLAUD_SCREEN_COORDS: 'wayland' };
  const fractional = capture({ rect: { x: d.x + 10.5, y: d.y + 10.2, width: 100.3, height: 50.1 } }, env).result;
  assert.deepEqual(pngSize(fractional), { width: 101, height: 51 });
  const rects = displays.map((x) => x.wayland);
  const maxX = Math.max(...rects.map((r) => r.x + r.width));
  const minY = Math.min(...rects.map((r) => r.y));
  // Clipping keeps the part inside the bounding box; uncovered pixels are black.
  const probe = { x: maxX - 30, y: minY - 5, width: 100, height: 15 };
  const { result } = capture({ rect: [...rects, probe] }, env);
  const images = result.slice(0, rects.length).map(decodePng);
  const clipped = decodePng(result[rects.length]);
  assert.deepEqual([clipped.width, clipped.height], [30, 10]);
  assert.ok(clipped.px.equals(expectedComposite(probe, rects, images).px));
  const { cb, messages } = collector();
  withEnv(env, () => {
    assertThrows(() => Recorder.captureScreenImage({ rect: { x: maxX + 10, y: minY, width: 5, height: 5 } }, cb), Error, 'Failed to capture screen image.');
  });
  assert.ok(messages().includes('CaptureScreenRectPng: requested rect is outside the virtual screen.'));
});

test('display modes, indexes and fallbacks return native-size PNGs', live, () => {
  const { displays } = layout('wayland');
  const sizes = displays.map((d) => `${d.pw}x${d.ph}`);
  const env = { PLAUD_SCREEN_COORDS: 'wayland' };
  const sizeOf = (buf) => {
    const { width, height } = pngSize(buf);
    return `${width}x${height}`;
  };
  for (const options of [undefined, {}, { mode: 'main' }, { mode: 'cursor' }, { requests: [{ mode: 'main' }] }]) {
    const buf = withEnv(env, () => Recorder.captureScreenImage(options));
    assert.ok(sizes.includes(sizeOf(buf)), `${JSON.stringify(options)} -> ${sizeOf(buf)}`);
  }
  const byIndex = capture({ displayIndex: displays.map((_, i) => i) }, env).result;
  const list = Array.isArray(byIndex) ? byIndex : [byIndex];
  assert.deepEqual(list.map(sizeOf), sizes);

  const single = capture([{ displayIndex: 0 }], env).result;
  assert.ok(Buffer.isBuffer(single), 'a one-request batch returns a Buffer');
  const pair = capture([{ displayIndex: 0 }, { mode: 'main' }], env).result;
  assert.ok(Array.isArray(pair) && pair.length === 2);

  const outOfRange = capture({ displayIndex: 99 }, env);
  assert.ok(sizes.includes(sizeOf(outOfRange.result)));
  assert.ok(outOfRange.messages().includes('displayIndex out of range; falling back to primary display.'));
  const unknownId = capture({ displayId: 4000000000 }, env);
  assert.ok(sizes.includes(sizeOf(unknownId.result)));
  assert.ok(unknownId.messages().includes('CaptureDisplayPngById: treating displayId as displayIndex on Linux.'));
  assert.ok(unknownId.messages().includes('displayId not found; falling back to primary display.'));

  // The callback is ignored when the first argument is undefined.
  const { cb, logs } = collector();
  withEnv(env, () => Recorder.captureScreenImage(undefined, cb));
  assert.equal(logs.length, 0);
  withEnv(env, () => Recorder.captureScreenImage(cb));
  assert.ok(logs.length > 0);
});

test('getWindowAtCursor returns null or the official shape; windowId captures it', live, (t) => {
  const { cb, logs } = collector();
  const w = withEnv({ PLAUD_SCREEN_COORDS: 'wayland' }, () => Recorder.getWindowAtCursor(cb));
  for (const { self, entry } of logs) {
    assert.equal(self, undefined);
    assert.ok(LEVELS.has(entry.level));
  }
  if (w === null) return t.skip('no window under the cursor');
  assert.deepEqual(Object.keys(w), ['windowId', 'x', 'y', 'width', 'height', 'ownerName', 'windowName']);
  assert.ok(Number.isInteger(w.windowId) && w.windowId > 0 && w.windowId <= 0xffffffff);
  for (const k of ['x', 'y', 'width', 'height']) assert.equal(typeof w[k], 'number');
  assert.ok(w.width >= 50 && w.height >= 50);
  assert.equal(typeof w.ownerName, 'string');
  assert.equal(typeof w.windowName, 'string');
  const { result } = capture({ windowId: w.windowId }, { PLAUD_SCREEN_COORDS: 'wayland' });
  const { width, height } = pngSize(result);
  assert.ok(Math.abs(width - w.width) <= Math.max(4, w.width * 0.05), `${width} vs ${w.width}`);
  assert.ok(Math.abs(height - w.height) <= Math.max(4, w.height * 0.05), `${height} vs ${w.height}`);
  assertThrows(() => withEnv({ PLAUD_SCREEN_COORDS: 'wayland' }, () => Recorder.captureScreenImage({ windowId: 0xfffffffe })), Error, 'Failed to capture screen image.');
});

// A forced backend is used as is or reported unusable, never replaced by
// another one: where the session cannot offer it (grim needs a Wayland
// compositor with a capture protocol, the portal a desktop portal with a
// Screenshot backend; CI's X11 session has neither), the permission check is
// denied with the reason and captures fail.
for (const backend of ['grim', 'portal']) {
  test(`forced ${backend} backend captures every display, or is reported unusable`, live, (t) => {
    const env = { PLAUD_SCREEN_COORDS: 'wayland', PLAUD_SCREEN_BACKEND: backend };
    const check = collector();
    const status = withEnv(env, () => Recorder.checkScreenCapturePermission(check.cb));
    if (status !== 0) {
      assert.equal(status, 1);
      const warnings = check.logs.filter((l) => l.entry.level === 'warn').map((l) => l.entry.message);
      assert.equal(warnings.length, 1, JSON.stringify(check.messages()));
      assert.match(warnings[0], /^No usable screen capture backend \(.+\)\.$/);
      t.diagnostic(`${backend}: ${warnings[0]}`);
      const failed = collector();
      withEnv(env, () => assertThrows(() => Recorder.captureScreenImage({ mode: 'main' }, failed.cb), Error,
        'Failed to capture screen image.'));
      assert.ok(failed.logs.some((l) => l.entry.level === 'error'), JSON.stringify(failed.messages()));
      return;
    }
    const { displays } = layout('wayland', backend);
    const runtime = process.env.XDG_RUNTIME_DIR;
    const listTemp = () => (runtime && fs.existsSync(path.join(runtime, 'hypr')) ? fs.readdirSync(path.join(runtime, 'hypr')).filter((n) => n.startsWith('xdph_screenshot')) : []);
    const before = new Set(listTemp());
    const { result, messages } = capture({ rect: displays.map((d) => d.wayland) }, env);
    const list = Array.isArray(result) ? result : [result];
    list.forEach((buf, i) => {
      const img = decodePng(buf);
      assert.deepEqual([img.width, img.height], [displays[i].pw, displays[i].ph]);
      assert.ok(distinctColors(img) > 1);
    });
    assert.ok(messages().some((m) => m.includes(`backends=${backend}, displays=`)), 'setup names the forced backend');
    assert.deepEqual(listTemp().filter((n) => !before.has(n)), [], 'portal temp files are removed');
  });
}
