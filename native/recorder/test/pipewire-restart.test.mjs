// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// PipeWire daemon loss and restart, on a private PipeWire instance that the
// tests kill and start again: a running recording reports one fatal
// device_invalidated event and recovers through restart(); a restart that
// cannot reconnect stops the recorder cleanly; without a daemon start()
// fails with init_failed; the device watcher and mute monitor reconnect.

import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import {
  audioUnavailableReason, createNullSink, createVirtualMic, loadAddon, pactl, parseOggPages, play, probe,
  runCleanups, sleep, startPrivatePipeWire, tmpFile, tone, uniqueName, waitFor, writeOgg, writeWav,
} from './support/pw.mjs';

const { Recorder } = loadAddon();
const skip = audioUnavailableReason();
let pw;
let toneWav;

before(async () => {
  if (skip) return;
  pw = await startPrivatePipeWire();
  toneWav = writeWav(tmpFile('tone.wav'), tone(1000, 60, 0.3));
});

after(runCleanups);

function collector() {
  const events = { log: [], audio: [], encoded: [], fatal: [] };
  const started = Date.now();
  const cb = (ev) => {
    if (events[ev.type]) events[ev.type].push({ ...ev, at: Date.now() - started });
  };
  return { events, cb };
}

async function devices(micName, outName) {
  const mic = createVirtualMic(micName, 'PlaudRestartMic');
  const out = createNullSink(outName, 'PlaudRestartOut');
  assert.ok(await waitFor(() => Recorder.listMicrophoneDevicesDetailed().some((d) => d.uid === micName)
    && Recorder.listSpeakerDevicesDetailed().some((d) => d.uid === outName)));
  play(out.name, toneWav);
  return { mic, out };
}

test('daemon crash mid-recording: one fatal device_invalidated, restart() recovers', { skip }, async (t) => {
  const micName = uniqueName('plaud_test_rs_mic');
  const outName = uniqueName('plaud_test_rs_out');
  await devices(micName, outName);
  await sleep(200);
  const r = new Recorder();
  // A recorder left running (failed assertion) would keep this process alive.
  t.after(() => r.stop());
  const { events, cb } = collector();
  r.start({
    enableMicrophone: true, enableSystemAudio: true, encoding: true,
    audioDevices: { microphone: micName, speaker: outName },
  }, cb);
  await sleep(1000);
  assert.ok(events.audio.length > 50);
  assert.deepEqual(events.fatal, []);

  await pw.stop();
  assert.ok(await waitFor(() => events.fatal.length > 0, 3000), 'fatal event after the daemon died');
  await sleep(300);
  assert.equal(events.fatal.length, 1, JSON.stringify(events.fatal));
  assert.equal(events.fatal[0].code, 'device_invalidated');
  assert.match(events.fatal[0].message, /^\[(mic|sys):env_device_invalidated\] /);

  await pw.start();
  await devices(micName, outName);
  const audioBefore = events.audio.length;
  assert.equal(r.restart(), true);
  await sleep(1000);
  const logs = events.log.map((l) => l.message);
  assert.ok(logs.includes('LinuxRecorder::Restart - recorder restarted successfully.'), logs.join('\n'));
  assert.ok(events.audio.length - audioBefore > 50, 'level events resume');
  assert.ok(events.audio.slice(audioBefore + 30).some((e) => e.systemVolume > 0.1), 'system audio flows again');
  assert.equal(r.stop(), true);

  const ogg = Buffer.concat(events.encoded.map((e) => Buffer.from(e.data)));
  const pages = parseOggPages(ogg);
  assert.equal(pages[0].body.toString('latin1', 0, 8), 'OpusHead');
  assert.equal(pages.at(-1).flags & 4, 4, 'end of stream written');
  const info = probe(writeOgg(ogg, 'restart.ogg'));
  assert.equal(info.streams.find((s) => s.codec_type === 'audio').codec_name, 'opus');
  assert.ok(Number(info.format.duration) > 1.5, `duration ${info.format.duration}`);
});

test('restart() inside the fatal callback (the official flow) recovers and re-pins the devices', { skip }, async (t) => {
  const micName = uniqueName('plaud_test_rs_cb_mic');
  const outName = uniqueName('plaud_test_rs_cb_out');
  await devices(micName, outName);
  await sleep(200);
  const r = new Recorder();
  t.after(() => r.stop());
  const events = { log: [], audio: [], encoded: [], fatal: [] };
  let restarted;
  r.start({
    enableMicrophone: true, enableSystemAudio: true, encoding: true,
    audioDevices: { microphone: micName, speaker: outName },
  }, (ev) => {
    if (events[ev.type]) events[ev.type].push(ev);
    // recordingService.handleRecorderFatal calls restart() synchronously here.
    if (ev.type === 'fatal' && ev.code === 'device_invalidated' && restarted === undefined) restarted = r.restart();
  });
  await sleep(800);
  pw.crash(600);
  assert.ok(await waitFor(() => restarted !== undefined, 8000), 'fatal event delivered');
  assert.equal(restarted, true);
  assert.equal(events.fatal.length, 1);
  await pw.waitReady();
  // The selected devices come back on the new daemon; the pinned streams move to them.
  await devices(micName, outName);
  const before = events.audio.length;
  await sleep(1500);
  assert.ok(events.audio.slice(before + 30).some((e) => e.systemVolume > 0.1), 'system audio from the re-created output');
  const logs = events.log.map((l) => l.message);
  assert.ok(logs.includes(`System audio source: monitor of PlaudRestartOut (${outName})`), logs.join('\n'));
  assert.equal(r.stop(), true);
});

test('restart() without a daemon fails after its retry budget and stops the recorder', { skip }, async (t) => {
  const r = new Recorder();
  t.after(() => r.stop());
  const { events, cb } = collector();
  r.start({ enableMicrophone: false, enableSystemAudio: true, encoding: true }, cb);
  await sleep(500);
  await pw.stop();
  assert.ok(await waitFor(() => events.fatal.length > 0, 3000));
  const t0 = Date.now();
  assert.equal(r.restart(), false);
  const took = Date.now() - t0;
  assert.ok(took >= 2500 && took < 6000, `restart took ${took} ms`);
  const logs = events.log.map((l) => l.message);
  assert.ok(logs.some((m) => m.startsWith('LinuxRecorder::Restart - restart failed: ')), logs.join('\n'));
  assert.equal(r.stop(), undefined, 'already stopped');
  assert.equal(r.restart(), false);
  const pages = parseOggPages(Buffer.concat(events.encoded.map((e) => Buffer.from(e.data))));
  assert.equal(pages.at(-1).flags & 4, 4, 'stream closed with end of stream');
  await pw.start();
});

test('without a daemon: start() fails with init_failed, permission is denied, lists are empty', { skip }, async () => {
  await pw.stop();
  try {
    const r = new Recorder();
    assert.throws(() => r.start({ enableMicrophone: true, enableSystemAudio: true }, () => {}),
      (e) => e instanceof Error && /^Recorder\.start failed: Failed to start recorder\. \[mic:init_failed\] /.test(e.message));
    assert.throws(() => r.start({ enableMicrophone: false, enableSystemAudio: true }, () => {}),
      (e) => /\[sys:init_failed\]/.test(e.message));
    const logs = [];
    assert.equal(Recorder.checkSystemAudioPermission((l) => logs.push(l)), 1);
    assert.equal(logs.length, 3, JSON.stringify(logs));
    assert.deepEqual(logs[0], { level: 'info', message: 'Starting system audio permission check' });
    assert.equal(logs[1].level, 'error');
    assert.match(logs[1].message, /^PipeWire is not reachable: /);
    assert.deepEqual(logs[2], { level: 'info', message: 'System audio permission status: Denied' });
    assert.equal(Recorder.requestSystemAudioPermission(), false);
    assert.deepEqual(Recorder.listMicrophoneDevicesDetailed(), []);
    assert.deepEqual(Recorder.listSpeakerDevices(), []);
  } finally {
    await pw.start();
  }
});

test('device watcher and mute monitor reconnect after a daemon restart', { skip }, async () => {
  const calls = [];
  const mute = [];
  assert.equal(Recorder.watchMicrophoneDevices((names) => calls.push(names)), true);
  Recorder.startSpeakerMuteMonitor((e) => mute.push(e));
  try {
    await pw.stop();
    await sleep(500);
    await pw.start();
    createVirtualMic(uniqueName('plaud_test_rs_late'), 'PlaudRestartLate');
    assert.ok(await waitFor(() => calls.some((l) => l.includes('PlaudRestartLate')), 8000), JSON.stringify(calls));

    const out = createNullSink(uniqueName('plaud_test_rs_mute'), 'PlaudRestartMute');
    pactl('set-default-sink', out.name);
    const id = (await waitFor(() => Recorder.listSpeakerDevicesDetailed().find((d) => d.uid === out.name && d.isDefault)))?.id;
    assert.ok(id !== undefined);
    assert.ok(await waitFor(() => mute.some((e) => e.type === 'log' && e.obj.message === `Added mute listener to device ${id}`), 4000),
      JSON.stringify(mute));
    pactl('set-sink-mute', out.name, '1');
    assert.ok(await waitFor(() => mute.some((e) => e.type === 'status' && e.obj.muted === true), 4000), JSON.stringify(mute));
    assert.equal(Recorder.isSpeakerMuted(), true);
  } finally {
    Recorder.stopWatchingMicrophoneDevices();
    Recorder.stopSpeakerMuteMonitor();
  }
});
