// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Argument validation, error messages and return shapes. Expected values
// are the official recorder_mac.node's answers to the same calls (recorded
// on macOS with node 26); cases where the official addon aborts the process
// instead of throwing are marked "Linux:" and expect a regular TypeError.
// Calls that reach PipeWire run against a private instance with one virtual
// microphone and one virtual output.

import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import {
  audioUnavailableReason, createNullSink, createVirtualMic, loadAddon, runCleanups, startPrivatePipeWire, uniqueName,
  waitFor,
} from './support/pw.mjs';

const { Recorder } = loadAddon();
const skip = audioUnavailableReason();
const cb = () => {};

before(async () => {
  if (skip) return;
  await startPrivatePipeWire();
  const mic = createVirtualMic(uniqueName('plaud_val_mic'), 'PlaudValidationMic');
  const out = createNullSink(uniqueName('plaud_val_out'), 'PlaudValidationOut');
  assert.ok(await waitFor(() => Recorder.listMicrophoneDevicesDetailed().some((d) => d.uid === mic.name)
    && Recorder.listSpeakerDevicesDetailed().some((d) => d.uid === out.name)));
});

after(runCleanups);

function outcome(fn) {
  try {
    const value = fn();
    return { value };
  } catch (e) {
    return { error: e.constructor.name, message: e.message };
  }
}

function check(label, fn, expected) {
  assert.deepEqual(outcome(fn), expected, label);
}

const typeError = (message) => ({ error: 'TypeError', message });
const error = (message) => ({ error: 'Error', message });
const value = (v) => ({ value: v });

test('methods on an idle recorder', () => {
  const r = new Recorder();
  check('pause(5)', () => r.pause(5), value(undefined));
  check('pause()', () => r.pause(), value(undefined));
  check('pause(-1)', () => r.pause(-1), value(undefined));
  check('pause(1.5)', () => r.pause(1.5), value(undefined));
  check('pause(null)', () => r.pause(null), typeError('pause expects an optional numeric mode.'));
  check('pause(undefined)', () => r.pause(undefined), typeError('pause expects an optional numeric mode.'));
  check("pause('x')", () => r.pause('x'), typeError('pause expects an optional numeric mode.'));
  check('begin(7)', () => r.begin(7), value(undefined));
  check('begin()', () => r.begin(), value(undefined));
  check('begin(null)', () => r.begin(null), typeError('begin expects an optional numeric mode.'));
  check('fillSilence(-5)', () => r.fillSilence(-5), value(undefined));
  check('fillSilence(0)', () => r.fillSilence(0), value(undefined));
  check('fillSilence(1.5)', () => r.fillSilence(1.5), value(undefined));
  check('fillSilence(NaN)', () => r.fillSilence(NaN), value(undefined));
  check('fillSilence()', () => r.fillSilence(), typeError('fillSilence requires a durationMs number argument.'));
  check("fillSilence('a')", () => r.fillSilence('a'), typeError('fillSilence requires a durationMs number argument.'));
  check('setApmEnabled(true)', () => r.setApmEnabled(true), value(undefined));
  check('setApmEnabled()', () => r.setApmEnabled(), typeError('setApmEnabled requires a single boolean argument.'));
  check('setApmEnabled(1)', () => r.setApmEnabled(1), typeError('setApmEnabled requires a single boolean argument.'));
  check('setApmEnabled(true,1)', () => r.setApmEnabled(true, 1),
    typeError('setApmEnabled requires a single boolean argument.'));
  check('setEncodingPaused()', () => r.setEncodingPaused(),
    typeError('setEncodingPaused requires a boolean argument.'));
  check('setEncodingPaused(1)', () => r.setEncodingPaused(1),
    typeError('setEncodingPaused requires a boolean argument.'));
  check('setEncodingPaused(true,1)', () => r.setEncodingPaused(true, 1), value(undefined));
  check('setEncodingPaused(false)', () => r.setEncodingPaused(false), value(undefined));
  check('stop()', () => r.stop(), value(undefined));
  check('stop(1)', () => r.stop(1), value(undefined));
  check('restart()', () => r.restart(), value(false));
  check('restart(1)', () => r.restart(1), value(false));
});

test('start() argument validation', () => {
  delete process.env.PLAUD_AEC_MODEL_PATH;
  delete process.env.PLAUD_ANS_MODEL_PATH;
  const r = new Recorder();
  const atLeastOne = typeError('At least one of enableMicrophone or enableSystemAudio must be true.');
  const unknown = (b) => typeError(`Unknown audioProcessing.backend: ${b}`);
  const requires = typeError('Start requires (options, callback).');
  check('start()', () => r.start(), requires);
  check('start({}, 5)', () => r.start({}, 5), requires);
  check('start(5, cb)', () => r.start(5, cb), requires);
  check('start(null, cb)', () => r.start(null, cb), requires);
  check('start(cb)', () => r.start(cb), requires);
  check('start([], cb)', () => r.start([], cb), atLeastOne);
  check('start({}, cb)', () => r.start({}, cb), atLeastOne);
  const cases = [
    [{ audioProcessing: { backend: 'x' } }, unknown('x')],
    [{ audioProcessing: { backend: 'plaud3a' } }, typeError('Plaud 3A selected but model paths are missing. Provide '
      + 'options.audioProcessing.plaudAecModelPath + plaudAnsModelPath (or env PLAUD_AEC_MODEL_PATH/PLAUD_ANS_MODEL_PATH).')],
    [{ audioProcessing: { backend: 'plaud3a', plaudAecModelPath: '/a', plaudAnsModelPath: '/b' } },
      error('Plaud 3A backend is not compiled in. Rebuild with PC_RECORDER_ENABLE_PLAUD_3A=ON.')],
    [{ audioProcessingBackend: 'x' }, unknown('x')],
    [{ audioProcessingBackend: 'none' }, atLeastOne],
    [{ audioProcessing: { backend: 5 } }, atLeastOne],
    [{ audioProcessing: 5, audioProcessingBackend: 'x' }, unknown('x')],
    [{ audioProcessing: { backend: 'webrtc' }, audioProcessingBackend: 'x' }, atLeastOne],
    [{ audioProcessing: {}, audioProcessingBackend: 'x' }, atLeastOne],
    [{ enableMicrophone: 'yes' }, atLeastOne],
    [{ encodingBitRate: -5 }, atLeastOne],
    [{ encodingBitRate: 'x' }, atLeastOne],
    [{ audioProcessing: { backend: 'WEBRTC' } }, atLeastOne],
    [{ audioProcessing: { backend: '' } }, unknown('')],
    [{ audioProcessing: { backend: 'apple' } }, unknown('apple')],
    [{ audioProcessing: { backend: 'vpio' } }, unknown('vpio')],
    [{ audioProcessing: { backend: 'off' } }, atLeastOne],
    [{ audioProcessing: { backend: 'disabled' } }, atLeastOne],
    [{ audioProcessing: { backend: 'apm' } }, atLeastOne],
    [{ audioProcessing: { backend: 'plaud-3a', plaudAecModelPath: '/a', plaudAnsModelPath: '/b' } },
      error('Plaud 3A backend is not compiled in. Rebuild with PC_RECORDER_ENABLE_PLAUD_3A=ON.')],
    [{ audioDevices: 5 }, atLeastOne],
    [{ audioDevices: null }, atLeastOne],
    [{ audioDevices: {} }, atLeastOne],
    [{ audioDevices: { microphone: 'uid-x', speaker: 7 } }, atLeastOne],
    // Linux: the official addon aborts the process (napi_throw with a pending exception) on these.
    [{ audioDevices: { microphone: -1 } },
      typeError('audioDevices.microphone must be a non-negative integer within uint32 range.')],
    [{ audioDevices: { microphone: 1.5 } }, typeError('audioDevices.microphone must be an integer.')],
    [{ audioDevices: { microphone: true } },
      typeError('audioDevices.microphone must be a number, string, object, null, or undefined.')],
    [{ audioDevices: { microphone: {} } },
      typeError('audioDevices.microphone object must include deviceId (number) or deviceUid (string).')],
    [{ audioDevices: { speaker: { id: -1 } } },
      typeError('audioDevices.speaker.id must be a non-negative integer within uint32 range.')],
    [{ audioDeviceId: -1 }, typeError('audioDeviceId must be a non-negative integer within uint32 range.')],
    [{ microphoneDeviceId: 2.5 }, typeError('microphoneDeviceId must be an integer.')],
    [{ speakerDeviceId: 4294967296 }, typeError('speakerDeviceId must be a non-negative integer within uint32 range.')],
  ];
  for (const [opts, expected] of cases) check(`start(${JSON.stringify(opts)})`, () => r.start(opts, cb), expected);
});

test('setAudioDevice() parsing and results', { skip }, () => {
  const r = new Recorder();
  const mics = Recorder.listMicrophoneDevicesDetailed();
  const spks = Recorder.listSpeakerDevicesDetailed();
  const objectMsg = 'device object must include deviceId (number) or deviceUid (string).';
  const pair = (microphone, speaker) => value({ microphone, speaker });
  check('setAudioDevice()', () => r.setAudioDevice(), pair(null, null));
  check('setAudioDevice(null)', () => r.setAudioDevice(null), pair(null, null));
  check('setAudioDevice(undefined)', () => r.setAudioDevice(undefined), pair(null, null));
  check('setAudioDevice(80)', () => r.setAudioDevice(80), value(80));
  check('setAudioDevice(0)', () => r.setAudioDevice(0), value(0));
  check("setAudioDevice('nonexistent')", () => r.setAudioDevice('nonexistent'), value('nonexistent'));
  check("setAudioDevice('')", () => r.setAudioDevice(''), value(''));
  check("setAudioDevice('999999')", () => r.setAudioDevice('999999'), value('999999'));
  check('setAudioDevice(1.5)', () => r.setAudioDevice(1.5), typeError('deviceId must be an integer.'));
  check('setAudioDevice(-1)', () => r.setAudioDevice(-1), typeError('deviceId must be within uint32 range.'));
  check('setAudioDevice(2**32)', () => r.setAudioDevice(4294967296), typeError('deviceId must be within uint32 range.'));
  check('setAudioDevice(NaN)', () => r.setAudioDevice(NaN), typeError('deviceId must be within uint32 range.'));
  check('setAudioDevice(true)', () => r.setAudioDevice(true),
    typeError('setAudioDevice expects number, string, object, or null.'));
  check('{deviceId:81}', () => r.setAudioDevice({ deviceId: 81 }), pair(81, null));
  check('{deviceUid:5}', () => r.setAudioDevice({ deviceUid: 5 }), typeError(objectMsg));
  check('{id:5}', () => r.setAudioDevice({ id: 5 }), pair(5, null));
  check('{id:-1}', () => r.setAudioDevice({ id: -1 }), typeError('device.id must be within uint32 range.'));
  check('{id:1.5}', () => r.setAudioDevice({ id: 1.5 }), typeError('device.id must be an integer.'));
  check("{deviceId:'x'}", () => r.setAudioDevice({ deviceId: 'x' }), typeError(objectMsg));
  check('{deviceId:1.5}', () => r.setAudioDevice({ deviceId: 1.5 }), typeError('device.deviceId must be an integer.'));
  check("{microphone:'nonexistent'}", () => r.setAudioDevice({ microphone: 'nonexistent' }), pair('nonexistent', null));
  check('{microphone:{deviceId:1.5}}', () => r.setAudioDevice({ microphone: { deviceId: 1.5 } }),
    typeError('microphone.deviceId must be an integer.'));
  check('{microphone:{}}', () => r.setAudioDevice({ microphone: {} }),
    typeError('microphone object must include deviceId (number) or deviceUid (string).'));
  check('{microphone:{id:2}}', () => r.setAudioDevice({ microphone: { id: 2 } }), pair(2, null));
  check('{microphone:true}', () => r.setAudioDevice({ microphone: true }),
    typeError('microphone must be a number, string, object, null, or undefined.'));
  check('{microphone:-1}', () => r.setAudioDevice({ microphone: -1 }),
    typeError('microphone must be within uint32 range.'));
  check('{speaker:true}', () => r.setAudioDevice({ speaker: true }),
    typeError('speaker must be a number, string, object, null, or undefined.'));
  check('{}', () => r.setAudioDevice({}), typeError(objectMsg));
  check('{microphone:null}', () => r.setAudioDevice({ microphone: null }), pair(null, null));
  check('[]', () => r.setAudioDevice([]), typeError(objectMsg));
  check('[1]', () => r.setAudioDevice([1]), typeError(objectMsg));
  check('{deviceId:5,deviceUid:x}', () => r.setAudioDevice({ deviceId: 5, deviceUid: 'x' }), pair(5, null));
  if (mics.length > 0) {
    const mic = mics[0];
    check('setAudioDevice(mic.uid)', () => r.setAudioDevice(mic.uid), value(mic.id));
    check('{deviceUid: mic.uid}', () => r.setAudioDevice({ deviceUid: mic.uid }), pair(mic.id, null));
    check('{uid: mic.uid}', () => r.setAudioDevice({ uid: mic.uid }), pair(mic.id, null));
    check('{microphone:{uid}}', () => r.setAudioDevice({ microphone: { uid: mic.uid } }), pair(mic.id, null));
    check('{microphone: uid, deviceId: 5}', () => r.setAudioDevice({ microphone: mic.uid, deviceId: 5 }),
      pair(mic.id, null));
  }
  if (spks.length > 0) {
    const spk = spks[0];
    check('{speaker: spk.uid}', () => r.setAudioDevice({ speaker: spk.uid }), pair(null, spk.id));
    check('{speaker: spk.id}', () => r.setAudioDevice({ speaker: spk.id }), pair(null, spk.id));
    // A sink uid is not a microphone: it stays unresolved on the microphone side.
    check('{microphone: spk.uid}', () => r.setAudioDevice({ microphone: spk.uid }), pair(spk.uid, null));
  }
});

test('device lists have the official shapes', { skip }, () => {
  for (const [names, detailed] of [
    [Recorder.listMicrophoneDevices(), Recorder.listMicrophoneDevicesDetailed()],
    [Recorder.listSpeakerDevices(), Recorder.listSpeakerDevicesDetailed()],
  ]) {
    assert.ok(Array.isArray(names) && Array.isArray(detailed));
    assert.equal(names.length, detailed.length);
    for (const [i, d] of detailed.entries()) {
      assert.deepEqual(Object.keys(d), ['id', 'name', 'uid', 'isDefault']);
      assert.equal(typeof d.id, 'number');
      assert.equal(typeof d.name, 'string');
      assert.equal(typeof d.uid, 'string');
      assert.equal(typeof d.isDefault, 'boolean');
      assert.equal(names[i], d.name);
    }
    if (detailed.length > 0) assert.equal(detailed.filter((d) => d.isDefault).length, 1, 'exactly one default');
  }
});

test('system audio permission statics', { skip }, () => {
  const logs = [];
  assert.equal(Recorder.checkSystemAudioPermission((l) => logs.push(l)), 0);
  assert.deepEqual(logs, [
    { level: 'info', message: 'Starting system audio permission check' },
    { level: 'info', message: 'System audio permission status: Granted' },
  ]);
  // The official addon aborts without a callback; Linux just skips the logs.
  assert.equal(Recorder.checkSystemAudioPermission(), 0);
  const requestLogs = [];
  assert.equal(Recorder.requestSystemAudioPermission((l) => requestLogs.push(l)), true);
  assert.deepEqual(requestLogs.map((l) => l.message), [
    'Starting system audio permission request',
    'System audio permission request result: granted',
  ]);
});

test('microphone watcher registration', { skip }, () => {
  check('watch(1)', () => Recorder.watchMicrophoneDevices(1), typeError('watchMicrophoneDevices requires a callback'));
  check('watch()', () => Recorder.watchMicrophoneDevices(), typeError('watchMicrophoneDevices requires a callback'));
  check('watch(cb)', () => Recorder.watchMicrophoneDevices(cb), value(true));
  check('watch(cb) again', () => Recorder.watchMicrophoneDevices(cb), value(true));
  check('stopWatching', () => Recorder.stopWatchingMicrophoneDevices(), value(true));
  check('stopWatching again', () => Recorder.stopWatchingMicrophoneDevices(), value(true));
});

test('speaker mute monitor lifecycle', { skip }, async () => {
  check('isSpeakerMuted before start', () => Recorder.isSpeakerMuted(), error('Speaker mute monitor is not started.'));
  check('stop before start', () => Recorder.stopSpeakerMuteMonitor(), value(undefined));
  check('start()', () => Recorder.startSpeakerMuteMonitor(),
    typeError('startSpeakerMuteMonitor requires a callback function.'));
  const events = [];
  check('start(cb)', () => Recorder.startSpeakerMuteMonitor((e) => events.push(e)), value(undefined));
  const muted = Recorder.isSpeakerMuted();
  assert.equal(typeof muted, 'boolean');
  check('start again', () => Recorder.startSpeakerMuteMonitor(cb), error('Speaker mute monitor already started.'));
  await new Promise((resolve) => setTimeout(resolve, 200));
  const sinks = Recorder.listSpeakerDevicesDetailed();
  const def = sinks.find((s) => s.isDefault);
  assert.ok(def, 'a default output exists on the test machine');
  assert.deepEqual(events, [
    { type: 'log', obj: { level: 'info', message: `Added mute listener to device ${def.id}` } },
    { type: 'log', obj: { level: 'info', message: 'LinuxSpeakerMuteMonitor started.' } },
    { type: 'status', obj: { muted } },
  ]);
  check('stop', () => Recorder.stopSpeakerMuteMonitor(), value(undefined));
  check('stop again', () => Recorder.stopSpeakerMuteMonitor(), value(undefined));
  check('isSpeakerMuted after stop', () => Recorder.isSpeakerMuted(), error('Speaker mute monitor is not started.'));
});
