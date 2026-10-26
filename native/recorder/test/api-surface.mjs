// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Every Recorder call the official Plaud Desktop JavaScript makes must exist
// on the Linux addon, and the class surface must match the official addon's
// (property order and function lengths as read from recorder_mac.node).
// The official JS (out-global-online) is read from PLAUD_RE_JS_DIR (an
// extracted app), else from the default reverse-engineering checkout, else
// from the app.asar of the release pinned in official.json that
// `npm run fetch-official` caches (or PLAUD_OFFICIAL_RESOURCES), as the
// repository's own test suite finds it.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { loadAddon } from './support/pw.mjs';

const require = createRequire(import.meta.url);

const { Recorder } = loadAddon();

const OFFICIAL_PROTOTYPE = ['start', 'begin', 'pause', 'stop', 'restart', 'setAudioDevice', 'setApmEnabled',
  'fillSilence', 'setEncodingPaused'];
const OFFICIAL_STATICS = ['checkSystemAudioPermission', 'requestSystemAudioPermission',
  'checkScreenCapturePermission', 'checkScreenCapturePermissionFresh', 'requestScreenCapturePermission',
  'captureScreenImage', 'getWindowAtCursor', 'extractTextFromImage', 'listMicrophoneDevices',
  'listMicrophoneDevicesDetailed', 'watchMicrophoneDevices', 'stopWatchingMicrophoneDevices', 'listSpeakerDevices',
  'listSpeakerDevicesDetailed', 'computeAecLightMetrics', 'startSpeakerMuteMonitor', 'stopSpeakerMuteMonitor',
  'isSpeakerMuted'];

const REPO_ROOT = fileURLToPath(new URL('../../../', import.meta.url));
const JS_DIR = 'out-global-online';
const isJs = (name) => /\.(c|m)?js$/.test(name);

function listJs(dir) {
  const out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) out.push(...listJs(p));
    else if (isJs(entry.name)) out.push(p);
  }
  return out;
}

/**
 * The official JS as [{ rel, src }] with where it came from, or { missing }
 * naming every place that was looked at.
 */
function officialSources() {
  const reRoot = process.env.PLAUD_RE_JS_DIR
    ?? path.join(os.homedir(), 'Personal', 'Sources', '.plaud-re', 'mac', 'app', JS_DIR);
  if (fs.existsSync(reRoot)) {
    return { from: reRoot, files: listJs(reRoot).map((f) => ({ rel: path.relative(reRoot, f), src: fs.readFileSync(f, 'utf8') })) };
  }
  if (process.env.PLAUD_RE_JS_DIR) return { missing: `${reRoot} (PLAUD_RE_JS_DIR)` };
  const { version } = JSON.parse(fs.readFileSync(path.join(REPO_ROOT, 'official.json'), 'utf8'));
  const resources = process.env.PLAUD_OFFICIAL_RESOURCES ?? path.join(REPO_ROOT, '.cache', 'official', version, 'Resources');
  const asarPath = path.join(resources, 'app.asar');
  if (!fs.existsSync(asarPath)) {
    return { missing: `${reRoot} or ${asarPath} (set PLAUD_RE_JS_DIR, or run "npm run fetch-official")` };
  }
  const { AsarArchive } = require(path.join(REPO_ROOT, 'patcher', 'lib', 'asar.js'));
  const archive = AsarArchive.open(asarPath);
  try {
    const files = archive.listFiles()
      .filter((p) => p.startsWith(`${JS_DIR}/`) && isJs(p))
      .map((p) => ({ rel: p.slice(JS_DIR.length + 1), src: archive.readFile(p).toString('utf8') }));
    return { from: `${asarPath}:${JS_DIR}`, files };
  } finally {
    archive.close();
  }
}

function officialCalls(files) {
  const statics = new Map();
  const instance = new Map();
  for (const { rel, src } of files) {
    for (const m of src.matchAll(/\bRecorder\.([A-Za-z_$][\w$]*)\s*\(/g)) {
      if (!statics.has(m[1])) statics.set(m[1], rel);
    }
    for (const m of src.matchAll(/\bthis\.recorder\.([A-Za-z_$][\w$]*)\s*\(/g)) {
      if (!instance.has(m[1])) instance.set(m[1], rel);
    }
  }
  return { statics, instance };
}

test('every Recorder call in the official JS exists on the addon', (t) => {
  const official = officialSources();
  if (official.missing) {
    t.skip(`official JS not found at ${official.missing}`);
    return;
  }
  const { statics, instance } = officialCalls(official.files);
  assert.ok(statics.size > 0, `no Recorder.<static>( calls found in ${official.from}`);
  assert.ok(instance.size > 0, `no this.recorder.<method>( calls found in ${official.from}`);
  for (const [name, file] of statics) {
    assert.equal(typeof Recorder[name], 'function', `static Recorder.${name} (called in ${file}) is missing`);
  }
  for (const [name, file] of instance) {
    assert.equal(typeof Recorder.prototype[name], 'function',
      `Recorder.prototype.${name} (called in ${file}) is missing`);
  }
  t.diagnostic(`official JS: ${official.files.length} files from ${official.from}`);
  t.diagnostic(`statics used by the app: ${[...statics.keys()].sort().join(', ')}`);
  t.diagnostic(`instance methods used by the app: ${[...instance.keys()].sort().join(', ')}`);
});

test('class surface matches the official addon', () => {
  const proto = Object.getOwnPropertyNames(Recorder.prototype).filter((k) => k !== 'constructor');
  assert.deepEqual(proto, OFFICIAL_PROTOTYPE);
  // Built-in own properties of a function object; V8 versions differ in whether a native class
  // constructor carries the legacy 'arguments'/'caller' pair (Node 24 does, Node 26 does not).
  const builtins = ['length', 'name', 'prototype', 'arguments', 'caller'];
  const statics = Object.getOwnPropertyNames(Recorder).filter((k) => !builtins.includes(k));
  assert.deepEqual(statics, OFFICIAL_STATICS);
  for (const k of OFFICIAL_PROTOTYPE) assert.equal(Recorder.prototype[k].length, 0, k);
  for (const k of OFFICIAL_STATICS) assert.equal(Recorder[k].length, 0, k);
  assert.equal(Recorder.name, 'Recorder');
  assert.throws(() => Recorder(), { name: 'TypeError', message: "Class constructors cannot be invoked without 'new'" });
  assert.ok(new Recorder() instanceof Recorder);
});

test('module exports only Recorder', () => {
  assert.deepEqual(Object.keys(loadAddon()), ['Recorder']);
});
