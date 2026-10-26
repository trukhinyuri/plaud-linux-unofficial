// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Acoustic echo cancellation through the real graph: the output's monitor is
// looped back (20 ms delay) into a virtual microphone, so the microphone
// hears exactly what the recorder also captures as system audio. With a
// loudspeaker-type output the WebRTC AEC must remove that echo; with a
// headphone-type output AEC is off and the echo stays. The differential
// measurement isolates AEC from noise suppression and gain control.

import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import {
  audioUnavailableReason, createEchoPath, createNullSink, createVirtualMic, db, loadAddon, noise, play, record,
  runCleanups, sleep, startPrivatePipeWire, tmpFile, uniqueName, waitFor, writeWav,
} from './support/pw.mjs';

const { Recorder } = loadAddon();
const skip = audioUnavailableReason();
let mic;
let speakers;
let headphones;
let talkWav;

/** Speech-like test signal: noise with a 4 Hz syllable envelope (NS keeps it). */
function speechLike(seconds) {
  const x = noise(seconds, 0.35, 11);
  for (let i = 0; i < x.length; i++) x[i] *= Math.abs(Math.sin((Math.PI * 4 * i) / 48000));
  return x;
}

before(async () => {
  if (skip) return;
  await startPrivatePipeWire();
  mic = createVirtualMic(uniqueName('plaud_test_aec_mic'), 'PlaudAecMic');
  speakers = createNullSink(uniqueName('plaud_test_aec_spk'), 'PlaudAecSpeakers');
  headphones = createNullSink(uniqueName('plaud_test_aec_hp'), 'PlaudAecHeadphones', 'device.form-factor=headphone');
  assert.ok(await waitFor(() => Recorder.listMicrophoneDevicesDetailed().some((d) => d.uid === mic.name)
    && Recorder.listSpeakerDevicesDetailed().filter((d) => [speakers.name, headphones.name].includes(d.uid)).length === 2));
  await createEchoPath(speakers.name, mic.feed);
  await createEchoPath(headphones.name, mic.feed);
  talkWav = writeWav(tmpFile('talk.wav'), speechLike(30));
});

after(runCleanups);

function energy(events, key, fromMs) {
  let acc = 0;
  for (const ev of events) {
    if (ev.at < fromMs) continue;
    const buf = ev[key];
    const f = new Float32Array(buf.buffer, buf.byteOffset, buf.length / 4);
    for (const v of f) acc += v * v;
  }
  return acc;
}

async function echoRun(output) {
  const p = play(output.name, talkWav);
  await sleep(300);
  const res = await record(Recorder, {
    enableMicrophone: true, enableSystemAudio: true, encoding: false, emitApmSignals: true,
    audioDevices: { microphone: mic.name, speaker: output.name }, audioProcessing: { backend: 'webrtc' },
  }, { durationMs: 6000 });
  p.kill();
  assert.equal(res.stopResult, true);
  assert.deepEqual(res.events.fatal, []);
  const mic2 = energy(res.events.apm, 'mic', 3000);
  const ref2 = energy(res.events.apm, 'lpb', 3000);
  const enh2 = energy(res.events.apm, 'enh', 3000);
  return { res, mic2, ref2, enh2, attenuationDb: db(enh2 / mic2) };
}

let withAec;

test('loudspeaker-type output: AEC removes the system-audio echo from the microphone', { skip }, async (t) => {
  withAec = await echoRun(speakers);
  t.diagnostic(`echo attenuation with AEC: ${withAec.attenuationDb.toFixed(1)} dB`);
  const logs = withAec.res.events.log.map((l) => l.message);
  assert.ok(logs.includes('Default output transport is loudspeaker-type output (enabling AEC if mixing)'),
    logs.join('\n'));
  assert.ok(withAec.mic2 > 1, `echo reached the microphone (energy ${withAec.mic2})`);
  assert.ok(withAec.ref2 > 1, `reference captured (energy ${withAec.ref2})`);
  assert.ok(withAec.attenuationDb <= -10, `echo attenuation ${withAec.attenuationDb.toFixed(1)} dB`);
});

test('headphone-type output: AEC is disabled and the microphone signal is kept', { skip }, async (t) => {
  const run = await echoRun(headphones);
  t.diagnostic(`attenuation without AEC: ${run.attenuationDb.toFixed(1)} dB`);
  const logs = run.res.events.log.map((l) => l.message);
  assert.ok(logs.includes(
    'Default output transport is headphone-type output (form factor headphone) (disabling AEC)'), logs.join('\n'));
  assert.ok(run.mic2 > 1, `microphone signal present (energy ${run.mic2})`);
  if (withAec) {
    assert.ok(run.attenuationDb >= withAec.attenuationDb + 10,
      `AEC accounts for >= 10 dB: with AEC ${withAec.attenuationDb.toFixed(1)} dB, without ${run.attenuationDb.toFixed(1)} dB`);
  }
});

test('no APM signals without audio processing', { skip }, async () => {
  const res = await record(Recorder, {
    enableMicrophone: true, enableSystemAudio: true, encoding: false, emitApmSignals: true,
    audioDevices: { microphone: mic.name, speaker: speakers.name }, audioProcessing: { backend: 'none' },
  }, { durationMs: 500 });
  assert.equal(res.stopResult, true);
  assert.equal(res.events.apm.length, 0);
  assert.ok(res.events.audio.length > 20);
});
