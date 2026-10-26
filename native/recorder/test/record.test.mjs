// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Recording through the real PipeWire graph: a virtual microphone and a
// virtual output (null sink) are fed with pw-play, recorded for 5 s, and the
// resulting Ogg/Opus is checked with ffprobe, decoded with ffmpeg and parsed
// page by page against the official container layout.

import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import {
  audioUnavailableReason, createNullSink, createVirtualMic, decodeOpus, loadAddon, noise, parseOggPages, play, probe,
  record, rms, runCleanups, sleep, startPrivatePipeWire, tmpFile, tone, tonePower, uniqueName, waitFor, writeOgg,
  writeWav,
} from './support/pw.mjs';

const { Recorder } = loadAddon();
const skip = audioUnavailableReason();

let mic;
let sink;
let micWav;
let sysWav;

/** Bursts of a tone (250 ms on / 250 ms off): non-stationary, so noise suppression keeps it. */
function bursts(freq, seconds, amp) {
  const x = tone(freq, seconds, amp);
  for (let i = 0; i < x.length; i++) if (Math.floor(i / 12000) % 2 === 1) x[i] = 0;
  return x;
}

before(async () => {
  if (skip) return;
  await startPrivatePipeWire();
  mic = createVirtualMic(uniqueName('plaud_test_mic'), 'PlaudTestMic');
  sink = createNullSink(uniqueName('plaud_test_out'), 'PlaudTestOut');
  micWav = writeWav(tmpFile('mic.wav'), bursts(440, 30, 0.4));
  sysWav = writeWav(tmpFile('sys.wav'), tone(1000, 30, 0.3));
  const ok = await waitFor(() => Recorder.listMicrophoneDevicesDetailed().some((d) => d.uid === mic.name)
    && Recorder.listSpeakerDevicesDetailed().some((d) => d.uid === sink.name));
  assert.ok(ok, 'virtual devices did not appear');
});

after(runCleanups);

function devices() {
  return { microphone: mic.name, speaker: sink.name };
}

test('5 s mixed recording: Ogg/Opus 48 kHz mono, right duration, both sources audible', { skip }, async () => {
  const p1 = play(mic.feed, micWav);
  const p2 = play(sink.name, sysWav);
  await sleep(300);
  const res = await record(Recorder, {
    enableMicrophone: true, enableSystemAudio: true, microphoneVolume: 1, systemVolume: 1,
    audioDevices: devices(), encoding: true, audioProcessing: { backend: 'webrtc' },
  }, { durationMs: 5000 });
  p1.kill();
  p2.kill();

  assert.deepEqual(res.startResult, { sampleRate: 48000, channels: 1, container: 'ogg' });
  assert.equal(res.stopResult, true);
  assert.deepEqual(res.events.fatal, []);
  const logs = res.events.log.map((l) => l.message);
  assert.ok(logs.includes('Recorder started successfully.'), logs.join('\n'));
  assert.ok(logs.includes('Recorder fully stopped and resources released.'));
  assert.ok(logs.some((m) => m === `Using user-selected microphone device: PlaudTestMic (${mic.name})`));
  assert.ok(logs.some((m) => m === `Using user-selected speaker device: PlaudTestOut (${sink.name})`));
  for (const l of res.events.log) assert.ok(['debug', 'info', 'warning', 'error'].includes(l.level), l.level);

  // Volume events every 10 ms, shaped like the official ones.
  const audio = res.events.audio;
  assert.ok(audio.length >= 450 && audio.length <= 520, `audio events: ${audio.length}`);
  const ev = audio[200];
  assert.deepEqual(Object.keys(ev).filter((k) => k !== 'at'),
    ['type', 'data', 'container', 'size', 'systemVolume', 'microphoneVolume']);
  assert.equal(ev.container, 'pcm');
  assert.equal(ev.size, 480);
  assert.equal(ev.data.length, 480 * 4);
  const late = audio.slice(100);
  assert.ok(Math.max(...late.map((e) => e.microphoneVolume)) > 0.1, 'microphone level');
  assert.ok(Math.min(...late.map((e) => e.systemVolume)) > 0.1, 'system audio level');

  // Encoded pages add up to the recording.
  const durations = res.events.encoded.reduce((a, e) => a + e.duration, 0);
  assert.ok(durations >= 4800 && durations <= 5600, `sum of page durations ${durations}`);
  for (const e of res.events.encoded) {
    assert.equal(e.container, 'ogg');
    assert.equal(e.size, e.data.length);
  }

  const file = writeOgg(res.ogg, 'mixed.ogg');
  const info = probe(file);
  const opus = info.streams.find((s) => s.codec_name === 'opus');
  assert.ok(opus, JSON.stringify(info.streams));
  assert.equal(opus.sample_rate, '48000');
  assert.equal(opus.channels, 1);
  const seconds = Number(info.format.duration);
  assert.ok(seconds >= 4.8 && seconds <= 5.6, `duration ${seconds}`);

  const pcm = decodeOpus(file);
  const from = 48000;
  const to = 48000 * 4.5;
  assert.ok(rms(pcm, from, to) > 0.05, `rms ${rms(pcm, from, to)}`);
  const p440 = tonePower(pcm, 440, from, to);
  const p1000 = tonePower(pcm, 1000, from, to);
  const pOff = tonePower(pcm, 2700, from, to);
  assert.ok(p1000 > 100 * pOff, `system audio tone missing (1000 Hz ${p1000}, off ${pOff})`);
  assert.ok(p440 > 100 * pOff, `microphone tone missing (440 Hz ${p440}, off ${pOff})`);
});

test('Ogg container layout matches the official encoder', { skip }, async () => {
  const p = play(mic.feed, micWav);
  await sleep(200);
  const res = await record(Recorder, {
    enableMicrophone: true, enableSystemAudio: false, audioDevices: devices(), encoding: true,
  }, { durationMs: 1500 });
  p.kill();
  const pages = parseOggPages(res.ogg);
  const [head, plaudBos, tags] = pages;
  const S = head.serial;
  assert.equal(head.flags, 2);
  assert.equal(head.granule, 0n);
  assert.equal(head.seq, 0);
  assert.equal(head.body.length, 19);
  assert.equal(head.body.toString('latin1', 0, 8), 'OpusHead');
  assert.deepEqual([...head.body.subarray(8)], [1, 1, 0, 0, 0x80, 0x3e, 0, 0, 0, 0, 0]);
  assert.equal(plaudBos.serial, (S + 1) >>> 0);
  assert.equal(plaudBos.flags, 2);
  assert.equal(plaudBos.seq, 0);
  assert.deepEqual(plaudBos.segments, [255, 45]);
  assert.equal(plaudBos.body.toString('latin1', 0, 8), 'PLAUD.AI');
  assert.ok(plaudBos.body.subarray(8).every((b) => b === 0));
  assert.equal(tags.serial, S);
  assert.equal(tags.seq, 1);
  assert.equal(tags.flags, 0);
  assert.equal(tags.body.toString('latin1', 0, 8), 'OpusTags');

  const rest = pages.slice(3);
  assert.ok(rest.length >= 8 && rest.length % 2 === 0, `audio/tag pages: ${rest.length}`);
  let granule = 0n;
  for (let i = 0; i < rest.length; i += 2) {
    const audio = rest[i];
    const tag = rest[i + 1];
    const last = i === rest.length - 2;
    assert.equal(audio.serial, S);
    assert.equal(audio.seq, 2 + i / 2);
    assert.equal(audio.packets.length, 18);
    for (const pkt of audio.packets) {
      assert.equal(pkt.length, 80, 'hard CBR at 32 kbit/s');
      // TOC: wideband 20 ms (SILK config 9 or CELT config 23), mono.
      assert.ok([9, 23].includes(pkt[0] >> 3), `TOC config ${pkt[0] >> 3}`);
      assert.equal((pkt[0] >> 2) & 1, 0, 'mono');
    }
    granule += 17280n;
    assert.equal(audio.granule, granule);
    assert.equal(tag.serial, (S + 1) >>> 0);
    assert.equal(tag.seq, 1 + i / 2);
    assert.deepEqual(tag.segments, [23]);
    assert.ok(tag.body.every((b) => b === 0));
    assert.equal(tag.granule, audio.granule);
    assert.equal(audio.flags, last ? 4 : 0, `audio page ${i / 2} flags`);
    assert.equal(tag.flags, last ? 4 : 0, `tag page ${i / 2} flags`);
  }
  // Each encoded event is exactly one page.
  assert.equal(res.events.encoded.length, pages.length);
});

test('encoding off: PCM container and volume events only', { skip }, async () => {
  const res = await record(Recorder, {
    enableMicrophone: true, enableSystemAudio: true, audioDevices: devices(), encoding: false,
  }, { durationMs: 600 });
  assert.equal(res.startResult.container, 'pcm');
  assert.equal(res.events.encoded.length, 0);
  assert.ok(res.events.audio.length > 30);
});

test('start() on a running recorder and unknown devices', { skip }, async (t) => {
  const r = new Recorder();
  // A recorder left running (failed assertion) would keep this process alive.
  t.after(() => r.stop());
  const logs = [];
  const result = r.start({
    enableMicrophone: true, enableSystemAudio: false, encoding: true,
    audioDevices: { microphone: 'no-such-microphone' },
  }, (ev) => { if (ev.type === 'log') logs.push(ev); });
  assert.equal(result.container, 'ogg');
  assert.throws(() => r.start({ enableMicrophone: true }, () => {}),
    { name: 'Error', message: 'Recorder is already running. Please stop it first.' });
  await sleep(200);
  assert.equal(r.stop(), true);
  assert.equal(r.stop(), undefined);
  assert.ok(logs.some((l) => l.level === 'warning'
    && l.message === 'Microphone device "no-such-microphone" is not available. Falling back to system default.'),
  JSON.stringify(logs));
});

test('recording of background noise only is not silent and has no fatal events', { skip }, async () => {
  const noiseWav = writeWav(tmpFile('noise.wav'), noise(10, 0.2, 7));
  const p = play(sink.name, noiseWav);
  await sleep(200);
  const res = await record(Recorder, {
    enableMicrophone: false, enableSystemAudio: true, audioDevices: devices(), encoding: true,
  }, { durationMs: 1200 });
  p.kill();
  assert.deepEqual(res.events.fatal, []);
  const pcm = decodeOpus(writeOgg(res.ogg, 'noise.ogg'));
  assert.ok(rms(pcm, 4800, 48000) > 0.05);
});
