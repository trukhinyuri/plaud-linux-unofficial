// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// begin()/pause()/restart() hammered from the JS thread while audio flows.
// The capture rings are single-consumer: only the worker may move their read
// index, other threads post drop requests. The ring counts consumer calls
// from any other thread and the engine logs them as an internal error, so a
// second consumer fails this test deterministically, not only when the race
// happens to hit.

import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import {
  audioUnavailableReason, createNullSink, createVirtualMic, loadAddon, parseOggPages, play, probe, runCleanups,
  sleep, startPrivatePipeWire, tmpFile, tone, uniqueName, waitFor, writeOgg, writeWav,
} from './support/pw.mjs';

const { Recorder } = loadAddon();
const skip = audioUnavailableReason();
let mic;
let sink;

before(async () => {
  if (skip) return;
  await startPrivatePipeWire();
  mic = createVirtualMic(uniqueName('plaud_test_hammer_mic'), 'PlaudHammerMic');
  sink = createNullSink(uniqueName('plaud_test_hammer_out'), 'PlaudHammerOut');
  assert.ok(await waitFor(() => Recorder.listMicrophoneDevicesDetailed().some((d) => d.uid === mic.name)
    && Recorder.listSpeakerDevicesDetailed().some((d) => d.uid === sink.name)));
  play(mic.feed, writeWav(tmpFile('hammer-mic.wav'), tone(440, 60, 0.4)));
  play(sink.name, writeWav(tmpFile('hammer-sys.wav'), tone(1000, 60, 0.3)));
  await sleep(300);
});

after(runCleanups);

test('begin/pause/restart hammered while audio flows: one ring consumer, audio intact', { skip }, async (t) => {
  const r = new Recorder();
  // A recorder left running (failed assertion) would keep this process alive.
  t.after(() => r.stop());
  const events = { log: [], audio: [], encoded: [], fatal: [] };
  r.start({
    enableMicrophone: true, enableSystemAudio: true, encoding: true,
    audioDevices: { microphone: mic.name, speaker: sink.name }, audioProcessing: { backend: 'webrtc' },
  }, (ev) => events[ev.type]?.push({ ...ev, at: Date.now() }));
  await sleep(500);

  const modes = [undefined, 0, 1, 2];
  let cycles = 0;
  let restarts = 0;
  const end = Date.now() + 4000;
  let nextRestart = Date.now() + 200;
  while (Date.now() < end) {
    const mode = modes[cycles % modes.length];
    if (mode === undefined) {
      r.pause();
      r.begin();
    } else {
      r.pause(mode);
      r.begin(mode);
    }
    r.begin();
    if (Date.now() >= nextRestart) {
      assert.equal(r.restart(), true, 'restart with a live daemon');
      restarts += 1;
      nextRestart = Date.now() + 250;
    }
    cycles += 1;
    // Yield now and then so events drain while the hammering goes on.
    if (cycles % 4 === 0) await sleep(1);
  }

  const settled = Date.now();
  await sleep(1500);
  assert.equal(r.stop(), true);

  const logs = events.log.map((l) => l.message);
  const violations = logs.filter((m) => m.includes('consumed from more than one thread'));
  assert.deepEqual(violations, [], violations.join('\n'));
  assert.deepEqual(events.fatal, []);
  assert.ok(cycles > 500, `pause/begin cycles: ${cycles}`);
  assert.ok(restarts >= 8, `restarts: ${restarts}`);
  assert.equal(logs.filter((m) => m === 'LinuxRecorder::Restart - recorder restarted successfully.').length, restarts);

  // Both sources flow again once the hammering stops.
  const tail = events.audio.filter((e) => e.at >= settled + 700);
  assert.ok(tail.length > 40, `audio events after the hammering: ${tail.length}`);
  assert.ok(tail.some((e) => e.microphoneVolume > 0.1), 'microphone audible after the hammering');
  assert.ok(tail.some((e) => e.systemVolume > 0.1), 'system audio audible after the hammering');

  // The file is one valid, continuous Ogg Opus stream.
  const ogg = Buffer.concat(events.encoded.map((e) => Buffer.from(e.data)));
  const pages = parseOggPages(ogg);
  const audioPages = pages.filter((p) => p.serial === pages[0].serial && p.granule > 0n);
  for (let i = 1; i < audioPages.length; i += 1) {
    assert.ok(audioPages[i].granule > audioPages[i - 1].granule, `granule decreases at audio page ${i}`);
  }
  assert.equal(pages.at(-1).flags & 4, 4, 'end of stream written');
  const info = probe(writeOgg(ogg, 'hammer.ogg'));
  assert.equal(info.streams.find((s) => s.codec_type === 'audio').codec_name, 'opus');
  const seconds = Number(info.format.duration);
  assert.ok(seconds > 1.5 && seconds < 7, `duration ${seconds}`);
});
