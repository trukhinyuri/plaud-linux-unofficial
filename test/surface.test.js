'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const surface = require('../patcher/lib/api-surface');
const { computeRuntimeId } = require('../patcher/lib/runtime-id');

const MAIN = `
const r = new nativeRecorder.Recorder(opts);
nativeRecorder.Recorder.listMicrophoneDevicesDetailed();
Recorder.captureScreenImage(options, cb);
recorder.start(); recorder?.pause(); this.recorder.stop();
detector.startMicMonitor(); getMeetingDetector()?.stopSpeakerMonitor();
obj["watchMicrophoneDevices"](cb); other.unrelatedCall();
`;
const BINARY = Buffer.concat([
  Buffer.from('\0listMicrophoneDevicesDetailed\0captureScreenImage\0watchMicrophoneDevices\0'),
  Buffer.from('\x01_ZN4node3fooEv\0CamelCaseName\0ab\0newNativeThing\0unrelatedCall\0'),
]);

test('extracts identifier-shaped strings from binaries', () => {
  const ids = surface.binaryIdentifiers(BINARY);
  for (const id of ['listMicrophoneDevicesDetailed', 'captureScreenImage', 'newNativeThing', 'unrelatedCall']) assert.ok(ids.has(id), id);
  assert.ok(!ids.has('CamelCaseName'), 'must start lower-case');
  assert.ok(!ids.has('ab'), 'too short');
  assert.ok(!ids.has('_ZN4node3fooEv'), 'mangled symbols are not identifiers');
});

test('collects native calls through the known receivers', () => {
  assert.deepEqual(surface.usedNativeCalls(MAIN), {
    recorderStatics: ['captureScreenImage', 'listMicrophoneDevicesDetailed'],
    recorderMethods: ['pause', 'start', 'stop'],
    monitorMethods: ['startMicMonitor', 'stopSpeakerMonitor'],
  });
});

function makeSurface(code, binary, nodeFiles = ['recorder_mac.node', 'audio_monitor_mac.node']) {
  return surface.computeSurface({ mainSources: new Map([['main.js', code]]), nativeBinaries: new Map([['recorder_mac.node', binary]]), nodeFiles });
}

const API = {
  classes: {
    Recorder: { statics: ['captureScreenImage', 'listMicrophoneDevicesDetailed', 'watchMicrophoneDevices'], prototype: ['start', 'pause', 'stop'] },
    AudioMonitor: { statics: [], prototype: ['startMicMonitor', 'stopSpeakerMonitor'] },
  },
};

test('a release identical to the baseline passes', () => {
  const s = makeSurface(MAIN, BINARY);
  assert.ok(s.candidates.includes('watchMicrophoneDevices'), 'bracket member access counts');
  assert.ok(s.candidates.includes('unrelatedCall'));
  assert.deepEqual(surface.checkSurface({ surface: s, baseline: s, nativeApi: API }), { ok: true, problems: [] });
});

test('new native calls, new native identifiers and new addons are refused', () => {
  const baseline = makeSurface(MAIN, BINARY);
  const next = makeSurface(
    `${MAIN}\nrecorder.setLoudness(1); Recorder.listCameras(); x.newNativeThing();`,
    BINARY,
    ['recorder_mac.node', 'audio_monitor_mac.node', 'camera_mac.node', 'node_modules/electron-app-universal-protocol-client/prebuilds/darwin-arm64/x.node'],
  );
  const { ok, problems } = surface.checkSurface({ surface: next, baseline, nativeApi: API });
  assert.equal(ok, false);
  assert.ok(problems.some((p) => p.includes('recorder.setLoudness()')), problems.join('\n'));
  assert.ok(problems.some((p) => p.includes('Recorder.listCameras()')));
  assert.ok(problems.some((p) => p.includes('"newNativeThing"')));
  assert.ok(problems.some((p) => p.includes('camera_mac.node')));
  assert.ok(!problems.some((p) => p.includes('universal-protocol-client')), 'the macOS-only protocol client prebuild is ignored');
});

test('a new identifier the Linux addon exports is accepted', () => {
  const baseline = makeSurface(MAIN, BINARY);
  const next = makeSurface(`${MAIN}\nx.newNativeThing();`, BINARY);
  const api = structuredClone(API);
  api.classes.Recorder.statics.push('newNativeThing');
  assert.equal(surface.checkSurface({ surface: next, baseline, nativeApi: api }).ok, true);
});

test('the runtime ID covers patcher, runtime, addon API and Electron', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'plaud-rid-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const patcherDir = path.join(dir, 'patcher');
  const runtimeDir = path.join(dir, 'runtime');
  fs.mkdirSync(path.join(patcherDir, 'lib'), { recursive: true });
  fs.mkdirSync(path.join(runtimeDir, 'node_modules'), { recursive: true });
  fs.writeFileSync(path.join(patcherDir, 'index.js'), 'a');
  fs.writeFileSync(path.join(patcherDir, 'lib', 'x.js'), 'b');
  fs.writeFileSync(path.join(runtimeDir, 'main.js'), 'c');
  const base = { patcherDir, runtimeDir, nativeApi: API, electronVersion: '36.7.4' };
  const id = computeRuntimeId(base);
  assert.match(id, /^[0-9a-f]{32}$/);
  assert.equal(computeRuntimeId(base), id, 'deterministic');

  fs.writeFileSync(path.join(runtimeDir, 'patch-manifest.json'), '{}');
  fs.writeFileSync(path.join(runtimeDir, 'node_modules', 'dep.js'), 'ignored');
  fs.writeFileSync(path.join(runtimeDir, 'notes.txt'), 'ignored');
  assert.equal(computeRuntimeId(base), id, 'generated files, dependencies and non-code files are ignored');

  fs.writeFileSync(path.join(patcherDir, 'lib', 'x.js'), 'b2');
  const changed = computeRuntimeId(base);
  assert.notEqual(changed, id);
  assert.notEqual(computeRuntimeId({ ...base, electronVersion: '36.7.5' }), changed);
  assert.notEqual(computeRuntimeId({ ...base, nativeApi: { classes: {} } }), changed);
});
