// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Device hot-plug: the microphone watcher reports plug/unplug, and a
// recording whose selected devices disappear falls back to the defaults
// without a gap, then returns to the selected devices when they come back.
// Runs in a private PipeWire instance with virtual devices only.

import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import {
  audioUnavailableReason, createNullSink, createVirtualMic, decodeOpus, loadAddon, overrideDefault, play, probe,
  record, runCleanups, sleep, startPrivatePipeWire, tmpFile, tone, tonePower, uniqueName, waitFor, writeOgg, writeWav,
} from './support/pw.mjs';

const { Recorder } = loadAddon();
const skip = audioUnavailableReason();
const sr = 48000;
let wavs;

before(async () => {
  if (skip) return;
  await startPrivatePipeWire();
  wavs = {
    mic: writeWav(tmpFile('mic.wav'), tone(440, 60, 0.3)),
    mic2: writeWav(tmpFile('mic2.wav'), tone(660, 60, 0.3)),
    out: writeWav(tmpFile('out.wav'), tone(1000, 60, 0.3)),
    out2: writeWav(tmpFile('out2.wav'), tone(1500, 60, 0.3)),
  };
});

after(runCleanups);

const has = (list, uid) => list.some((d) => d.uid === uid);

test('watchMicrophoneDevices reports microphones being plugged and unplugged', { skip }, async () => {
  const calls = [];
  assert.equal(Recorder.watchMicrophoneDevices((names) => calls.push(names)), true);
  try {
    const m = createVirtualMic(uniqueName('plaud_test_hot'), 'PlaudHotPlug');
    assert.ok(await waitFor(() => calls.some((l) => l.includes('PlaudHotPlug'))), JSON.stringify(calls));
    const n = calls.length;
    m.unload();
    assert.ok(await waitFor(() => calls.length > n && !calls.at(-1).includes('PlaudHotPlug')), JSON.stringify(calls));
    for (const l of calls) assert.ok(Array.isArray(l) && l.every((s) => typeof s === 'string'));
    // An output appearing is not a microphone change.
    await sleep(400);
    const settled = calls.length;
    const s = createNullSink(uniqueName('plaud_test_hot_out'), 'PlaudHotPlugOut');
    await sleep(600);
    assert.equal(calls.length, settled, 'no callback for a new output');
    s.unload();
  } finally {
    assert.equal(Recorder.stopWatchingMicrophoneDevices(), true);
  }
  await sleep(300);
  const stopped = calls.length;
  const late = createVirtualMic(uniqueName('plaud_test_hot'), 'PlaudHotPlugLate');
  await sleep(600);
  assert.equal(calls.length, stopped, 'no callbacks after stopWatchingMicrophoneDevices');
  late.unload();
});

test('selected devices unplugged and re-plugged mid-recording: fallback, no gap, return', { skip }, async () => {
  const micName = uniqueName('plaud_test_hot_mic');
  const outName = uniqueName('plaud_test_hot_out');
  let mic = createVirtualMic(micName, 'PlaudHotMic');
  let out = createNullSink(outName, 'PlaudHotOut');
  const mic2 = createVirtualMic(uniqueName('plaud_test_hot_mic2'), 'PlaudHotMic2');
  const out2 = createNullSink(uniqueName('plaud_test_hot_out2'), 'PlaudHotOut2');
  assert.ok(await waitFor(() => {
    const mics = Recorder.listMicrophoneDevicesDetailed();
    const outs = Recorder.listSpeakerDevicesDetailed();
    return has(mics, micName) && has(mics, mic2.name) && has(outs, outName) && has(outs, out2.name);
  }));
  const defSource = overrideDefault('source', mic2.name);
  const defSink = overrideDefault('sink', out2.name);
  try {
    play(mic.feed, wavs.mic);
    play(out.name, wavs.out);
    play(mic2.feed, wavs.mic2);
    play(out2.name, wavs.out2);
    await sleep(300);
    const res = await record(Recorder, {
      enableMicrophone: true, enableSystemAudio: true, encoding: true, audioProcessing: { backend: 'none' },
      audioDevices: { microphone: micName, speaker: outName },
    }, {
      durationMs: 5000,
      actions: [
        {
          at: 1200,
          run: () => {
            mic.unload();
            out.unload();
          },
        },
        {
          at: 2800,
          run: () => {
            mic = createVirtualMic(micName, 'PlaudHotMic');
            out = createNullSink(outName, 'PlaudHotOut');
            play(mic.feed, wavs.mic);
            play(out.name, wavs.out);
          },
        },
      ],
    });
    assert.equal(res.stopResult, true);
    assert.deepEqual(res.events.fatal, []);
    const logs = res.events.log.map((l) => l.message);
    const micLog = logs.filter((m) => m.startsWith('Microphone source: '));
    const outLog = logs.filter((m) => m.startsWith('System audio source: '));
    assert.deepEqual(micLog, [
      `Microphone source: PlaudHotMic (${micName})`,
      `Microphone source: PlaudHotMic2 (${mic2.name})`,
      `Microphone source: PlaudHotMic (${micName})`,
    ], logs.join('\n'));
    assert.deepEqual(outLog, [
      `System audio source: monitor of PlaudHotOut (${outName})`,
      `System audio source: monitor of PlaudHotOut2 (${out2.name})`,
      `System audio source: monitor of PlaudHotOut (${outName})`,
    ], logs.join('\n'));

    // Level events keep flowing on the fallback devices.
    const fallback = res.events.audio.filter((e) => e.at >= 1600 && e.at < 2700);
    assert.ok(fallback.length > 80, `events during fallback: ${fallback.length}`);
    assert.ok(fallback.filter((e) => e.microphoneVolume > 0.05 && e.systemVolume > 0.05).length > 0.9 * fallback.length);

    const file = writeOgg(res.ogg, 'hotplug.ogg');
    const seconds = Number(probe(file).format.duration);
    assert.ok(seconds >= 4.8 && seconds <= 5.6, `duration ${seconds}`);
    const pcm = decodeOpus(file);
    const p = (f, a, b) => tonePower(pcm, f, a * sr, b * sr);
    assert.ok(p(440, 0.3, 1.1) > 100 * p(660, 0.3, 1.1) && p(1000, 0.3, 1.1) > 100 * p(1500, 0.3, 1.1), 'selected');
    assert.ok(p(660, 1.6, 2.6) > 100 * p(440, 1.6, 2.6) && p(1500, 1.6, 2.6) > 100 * p(1000, 1.6, 2.6), 'fallback');
    assert.ok(p(440, 3.6, 4.6) > 100 * p(660, 3.6, 4.6) && p(1000, 3.6, 4.6) > 100 * p(1500, 3.6, 4.6), 'returned');
  } finally {
    defSink.restore();
    defSource.restore();
  }
});
