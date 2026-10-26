// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// pause()/begin(), fillSilence() and setEncodingPaused() on a live recording.

import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import {
  audioUnavailableReason, createNullSink, createVirtualMic, decodeOpus, loadAddon, play, probe, record, rms,
  runCleanups, sleep, startPrivatePipeWire, tmpFile, tone, tonePower, uniqueName, waitFor, writeOgg, writeWav,
} from './support/pw.mjs';

const { Recorder } = loadAddon();
const skip = audioUnavailableReason();
let mic;
let sink;

before(async () => {
  if (skip) return;
  await startPrivatePipeWire();
  mic = createVirtualMic(uniqueName('plaud_test_mic'), 'PlaudPauseMic');
  sink = createNullSink(uniqueName('plaud_test_out'), 'PlaudPauseOut');
  assert.ok(await waitFor(() => Recorder.listMicrophoneDevicesDetailed().some((d) => d.uid === mic.name)
    && Recorder.listSpeakerDevicesDetailed().some((d) => d.uid === sink.name)));
  // Continuous tones: 440 Hz into the microphone, 1000 Hz into the output.
  play(mic.feed, writeWav(tmpFile('mic.wav'), tone(440, 60, 0.4)));
  play(sink.name, writeWav(tmpFile('sys.wav'), tone(1000, 60, 0.3)));
  await sleep(300);
});

after(runCleanups);

const options = () => ({
  enableMicrophone: true, enableSystemAudio: true, encoding: true,
  audioDevices: { microphone: mic.name, speaker: sink.name }, audioProcessing: { backend: 'none' },
});

const within = (events, from, to) => events.filter((e) => e.at >= from && e.at < to);

test('pause() + fillSilence() + begin(): gap becomes silence, timeline stays continuous', { skip }, async () => {
  const res = await record(Recorder, options(), {
    durationMs: 3500,
    actions: [
      { at: 1000, run: (r) => r.pause() },
      {
        at: 2000,
        run: (r) => {
          r.fillSilence(-5);
          r.fillSilence(Number.NaN);
          r.fillSilence(0);
          r.fillSilence(1000);
          r.begin();
        },
      },
    ],
  });
  assert.equal(res.stopResult, true);
  assert.deepEqual(res.events.fatal, []);
  assert.equal(within(res.events.audio, 1150, 1950).length, 0, 'no capture while paused');
  assert.ok(within(res.events.audio, 2300, 3300).length > 50, 'capture resumed');

  const file = writeOgg(res.ogg, 'paused.ogg');
  const seconds = Number(probe(file).format.duration);
  assert.ok(seconds >= 3.3 && seconds <= 3.95, `duration ${seconds}`);
  const pcm = decodeOpus(file);
  const sr = 48000;
  assert.ok(rms(pcm, 0.2 * sr, 0.9 * sr) > 0.1, 'audio before pause');
  assert.ok(rms(pcm, 1.15 * sr, 1.9 * sr) < 0.005, `silence in gap: ${rms(pcm, 1.15 * sr, 1.9 * sr)}`);
  assert.ok(rms(pcm, 2.3 * sr, 3.2 * sr) > 0.1, 'audio after begin');
});

test('pause(0)/begin(0) mute only the microphone, pause(1)/begin(1) only system audio', { skip }, async () => {
  const res = await record(Recorder, options(), {
    durationMs: 4400,
    actions: [
      { at: 600, run: (r) => r.pause(0) },
      { at: 1600, run: (r) => r.begin(0) },
      { at: 2400, run: (r) => r.pause(1) },
      { at: 3400, run: (r) => r.begin(1) },
    ],
  });
  assert.equal(res.stopResult, true);
  const micPaused = within(res.events.audio, 800, 1500);
  assert.ok(micPaused.length > 40);
  assert.ok(micPaused.every((e) => e.microphoneVolume === 0), 'microphone silent while paused');
  assert.ok(micPaused.every((e) => e.systemVolume > 0.1), 'system audio continues');
  const sysPaused = within(res.events.audio, 2600, 3300);
  assert.ok(sysPaused.length > 40);
  assert.ok(sysPaused.every((e) => e.systemVolume === 0), 'system audio silent while paused');
  assert.ok(sysPaused.every((e) => e.microphoneVolume > 0.1), 'microphone continues');
  assert.ok(within(res.events.audio, 3600, 4300).every((e) => e.systemVolume > 0.1 && e.microphoneVolume > 0.1));

  const pcm = decodeOpus(writeOgg(res.ogg, 'partial.ogg'));
  const sr = 48000;
  const p440Paused = tonePower(pcm, 440, 0.8 * sr, 1.5 * sr);
  const p440Live = tonePower(pcm, 440, 1.8 * sr, 2.3 * sr);
  const p1000Paused = tonePower(pcm, 1000, 2.6 * sr, 3.3 * sr);
  const p1000Live = tonePower(pcm, 1000, 3.6 * sr, 4.2 * sr);
  assert.ok(p440Paused < 0.01 * p440Live, `440 Hz paused/live ${p440Paused}/${p440Live}`);
  assert.ok(p1000Paused < 0.01 * p1000Live, `1000 Hz paused/live ${p1000Paused}/${p1000Live}`);
});

test('setEncodingPaused(true) stops the file but not the level events', { skip }, async () => {
  const res = await record(Recorder, options(), {
    durationMs: 3000,
    actions: [
      { at: 1000, run: (r) => r.setEncodingPaused(true) },
      { at: 2000, run: (r) => r.setEncodingPaused(false) },
    ],
  });
  assert.equal(res.stopResult, true);
  const during = within(res.events.audio, 1100, 1950);
  assert.ok(during.length > 60, 'level events continue while encoding is paused');
  assert.ok(during.every((e) => e.systemVolume > 0.1));
  assert.equal(within(res.events.encoded, 1100, 1950).length, 0, 'no pages while encoding is paused');
  const seconds = Number(probe(writeOgg(res.ogg, 'encpaused.ogg')).format.duration);
  assert.ok(seconds >= 1.8 && seconds <= 2.45, `duration ${seconds}`);
});

test('setApmEnabled() toggles processing on a running WebRTC recorder', { skip }, async () => {
  const res = await record(Recorder, { ...options(), audioProcessing: { backend: 'webrtc' }, emitApmSignals: true }, {
    durationMs: 1600,
    actions: [
      { at: 500, run: (r) => r.setApmEnabled(false) },
      { at: 1000, run: (r) => r.setApmEnabled(true) },
    ],
  });
  assert.equal(res.stopResult, true);
  const ev = res.events.apm[0];
  assert.deepEqual(Object.keys(ev).filter((k) => k !== 'at'), ['type', 'lpb', 'mic', 'enh', 'lpbSampleRate',
    'micSampleRate', 'enhSampleRate', 'lpbChannels', 'micChannels', 'enhChannels']);
  assert.deepEqual([ev.lpbSampleRate, ev.micSampleRate, ev.enhSampleRate], [48000, 48000, 48000]);
  assert.deepEqual([ev.lpbChannels, ev.micChannels, ev.enhChannels], [1, 1, 1]);
  for (const k of ['lpb', 'mic', 'enh']) assert.equal(ev[k].length, 480 * 4, k);
  assert.ok(within(res.events.apm, 100, 450).length > 20, 'APM signals while enabled');
  assert.equal(within(res.events.apm, 600, 950).length, 0, 'no APM signals while disabled');
  assert.ok(within(res.events.apm, 1100, 1550).length > 20, 'APM signals after re-enabling');
});
