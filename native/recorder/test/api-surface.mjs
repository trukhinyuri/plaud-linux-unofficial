// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Every Recorder call the official Plaud Desktop JavaScript makes must exist
// on the Linux addon, and the class surface must match the official addon's
// (property order and function lengths as read from recorder_mac.node).
// PLAUD_RE_JS_DIR points at the extracted official app (out-global-online).

import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import { loadAddon } from './support/pw.mjs';

const { Recorder } = loadAddon();

const OFFICIAL_PROTOTYPE = ['start', 'begin', 'pause', 'stop', 'restart', 'setAudioDevice', 'setApmEnabled',
  'fillSilence', 'setEncodingPaused'];
const OFFICIAL_STATICS = ['checkSystemAudioPermission', 'requestSystemAudioPermission',
  'checkScreenCapturePermission', 'checkScreenCapturePermissionFresh', 'requestScreenCapturePermission',
  'captureScreenImage', 'getWindowAtCursor', 'extractTextFromImage', 'listMicrophoneDevices',
  'listMicrophoneDevicesDetailed', 'watchMicrophoneDevices', 'stopWatchingMicrophoneDevices', 'listSpeakerDevices',
  'listSpeakerDevicesDetailed', 'computeAecLightMetrics', 'startSpeakerMuteMonitor', 'stopSpeakerMuteMonitor',
  'isSpeakerMuted'];

const reRoot = process.env.PLAUD_RE_JS_DIR
  ?? path.join(os.homedir(), 'Personal', 'Sources', '.plaud-re', 'mac', 'app', 'out-global-online');

function listJs(dir) {
  const out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) out.push(...listJs(p));
    else if (/\.(c|m)?js$/.test(entry.name)) out.push(p);
  }
  return out;
}

function officialCalls() {
  const statics = new Map();
  const instance = new Map();
  for (const file of listJs(reRoot)) {
    const src = fs.readFileSync(file, 'utf8');
    const rel = path.relative(reRoot, file);
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
  if (!fs.existsSync(reRoot)) {
    t.skip(`official JS not found at ${reRoot} (set PLAUD_RE_JS_DIR)`);
    return;
  }
  const { statics, instance } = officialCalls();
  assert.ok(statics.size > 0, 'no Recorder.<static>( calls found: wrong PLAUD_RE_JS_DIR?');
  assert.ok(instance.size > 0, 'no this.recorder.<method>( calls found: wrong PLAUD_RE_JS_DIR?');
  for (const [name, file] of statics) {
    assert.equal(typeof Recorder[name], 'function', `static Recorder.${name} (called in ${file}) is missing`);
  }
  for (const [name, file] of instance) {
    assert.equal(typeof Recorder.prototype[name], 'function',
      `Recorder.prototype.${name} (called in ${file}) is missing`);
  }
  t.diagnostic(`statics used by the app: ${[...statics.keys()].sort().join(', ')}`);
  t.diagnostic(`instance methods used by the app: ${[...instance.keys()].sort().join(', ')}`);
});

test('class surface matches the official addon', () => {
  const proto = Object.getOwnPropertyNames(Recorder.prototype).filter((k) => k !== 'constructor');
  assert.deepEqual(proto, OFFICIAL_PROTOTYPE);
  const statics = Object.getOwnPropertyNames(Recorder).filter((k) => !['length', 'name', 'prototype'].includes(k));
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
