// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Default-device recordings follow default changes mid-recording, the
// speaker mute monitor follows the default output, and a muted output is
// recorded as silence. Runs in a private PipeWire instance, so changing the
// defaults never affects the session.

import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';

import {
  audioUnavailableReason, createNullSink, createVirtualMic, decodeOpus, loadAddon, overrideDefault, pactl, play,
  record, runCleanups, sleep, startPrivatePipeWire, tmpFile, tone, tonePower, uniqueName, waitFor, writeOgg, writeWav,
} from './support/pw.mjs';

const { Recorder } = loadAddon();
const skip = audioUnavailableReason();
let sinkA;
let sinkB;
let micA;
let micB;

before(async () => {
  if (skip) return;
  await startPrivatePipeWire();
  sinkA = createNullSink(uniqueName('plaud_test_a'), 'PlaudSwitchA');
  sinkB = createNullSink(uniqueName('plaud_test_b'), 'PlaudSwitchB');
  micA = createVirtualMic(uniqueName('plaud_test_mic_a'), 'PlaudSwitchMicA');
  micB = createVirtualMic(uniqueName('plaud_test_mic_b'), 'PlaudSwitchMicB');
  assert.ok(await waitFor(() => {
    const spk = Recorder.listSpeakerDevicesDetailed().map((d) => d.uid);
    const mics = Recorder.listMicrophoneDevicesDetailed().map((d) => d.uid);
    return spk.includes(sinkA.name) && spk.includes(sinkB.name) && mics.includes(micA.name) && mics.includes(micB.name);
  }));
  play(sinkA.name, writeWav(tmpFile('a.wav'), tone(500, 60, 0.3)));
  play(sinkB.name, writeWav(tmpFile('b.wav'), tone(1500, 60, 0.3)));
  play(micA.feed, writeWav(tmpFile('ma.wav'), tone(700, 60, 0.3)));
  play(micB.feed, writeWav(tmpFile('mb.wav'), tone(1300, 60, 0.3)));
  await sleep(300);
});

after(runCleanups);

const defaultUid = (list) => list.find((d) => d.isDefault)?.uid;
const sr = 48000;

test('system audio follows a default output change mid-recording', { skip }, async () => {
  const def = overrideDefault('sink', sinkA.name);
  try {
    assert.ok(await waitFor(() => defaultUid(Recorder.listSpeakerDevicesDetailed()) === sinkA.name));
    const res = await record(Recorder, {
      enableMicrophone: false, enableSystemAudio: true, encoding: true, audioProcessing: { backend: 'none' },
    }, { durationMs: 3600, actions: [{ at: 1600, run: () => def.set(sinkB.name) }] });
    assert.equal(res.stopResult, true);
    assert.deepEqual(res.events.fatal, []);
    const logs = res.events.log.map((l) => l.message);
    assert.ok(logs.includes(`System audio source: monitor of PlaudSwitchA (${sinkA.name})`), logs.join('\n'));
    assert.ok(logs.includes(`System audio source: monitor of PlaudSwitchB (${sinkB.name})`), logs.join('\n'));
    const pcm = decodeOpus(writeOgg(res.ogg, 'sink-switch.ogg'));
    const before500 = tonePower(pcm, 500, 0.3 * sr, 1.4 * sr);
    const before1500 = tonePower(pcm, 1500, 0.3 * sr, 1.4 * sr);
    const after500 = tonePower(pcm, 500, 2.2 * sr, 3.4 * sr);
    const after1500 = tonePower(pcm, 1500, 2.2 * sr, 3.4 * sr);
    assert.ok(before500 > 100 * before1500, `before switch: 500 Hz ${before500}, 1500 Hz ${before1500}`);
    assert.ok(after1500 > 100 * after500, `after switch: 500 Hz ${after500}, 1500 Hz ${after1500}`);
    // No gap: the level never drops out for more than a few chunks around the switch.
    const quiet = res.events.audio.filter((e) => e.at > 300 && e.systemVolume < 0.05).length;
    assert.ok(quiet <= 15, `quiet chunks around the switch: ${quiet}`);
  } finally {
    def.restore();
  }
});

test('microphone follows a default input change mid-recording', { skip }, async () => {
  const def = overrideDefault('source', micA.name);
  try {
    assert.ok(await waitFor(() => defaultUid(Recorder.listMicrophoneDevicesDetailed()) === micA.name));
    const res = await record(Recorder, {
      enableMicrophone: true, enableSystemAudio: false, encoding: true, audioProcessing: { backend: 'none' },
    }, { durationMs: 3600, actions: [{ at: 1600, run: () => def.set(micB.name) }] });
    assert.equal(res.stopResult, true);
    assert.deepEqual(res.events.fatal, []);
    const logs = res.events.log.map((l) => l.message);
    assert.ok(logs.includes(`Microphone source: PlaudSwitchMicA (${micA.name})`), logs.join('\n'));
    assert.ok(logs.includes(`Microphone source: PlaudSwitchMicB (${micB.name})`), logs.join('\n'));
    const pcm = decodeOpus(writeOgg(res.ogg, 'source-switch.ogg'));
    const before700 = tonePower(pcm, 700, 0.3 * sr, 1.4 * sr);
    const before1300 = tonePower(pcm, 1300, 0.3 * sr, 1.4 * sr);
    const after700 = tonePower(pcm, 700, 2.2 * sr, 3.4 * sr);
    const after1300 = tonePower(pcm, 1300, 2.2 * sr, 3.4 * sr);
    assert.ok(before700 > 100 * before1300, `before switch: 700 Hz ${before700}, 1300 Hz ${before1300}`);
    assert.ok(after1300 > 100 * after700, `after switch: 700 Hz ${after700}, 1300 Hz ${after1300}`);
  } finally {
    def.restore();
  }
});

test('an explicitly selected non-default output stays pinned across default changes', { skip }, async () => {
  const def = overrideDefault('sink', sinkB.name);
  try {
    const res = await record(Recorder, {
      enableMicrophone: false, enableSystemAudio: true, encoding: true, audioProcessing: { backend: 'none' },
      audioDevices: { speaker: sinkA.name },
    }, { durationMs: 2400, actions: [{ at: 800, run: () => def.set(micA.feed) }] });
    assert.equal(res.stopResult, true);
    const pcm = decodeOpus(writeOgg(res.ogg, 'pinned.ogg'));
    const p500 = tonePower(pcm, 500, 1.4 * sr, 2.2 * sr);
    const others = tonePower(pcm, 1500, 1.4 * sr, 2.2 * sr) + tonePower(pcm, 700, 1.4 * sr, 2.2 * sr);
    assert.ok(p500 > 100 * others, `pinned output kept: 500 Hz ${p500}, others ${others}`);
  } finally {
    def.restore();
  }
});

test('an explicitly selected output that is the current default stays pinned too', { skip }, async () => {
  const def = overrideDefault('sink', sinkA.name);
  try {
    assert.ok(await waitFor(() => defaultUid(Recorder.listSpeakerDevicesDetailed()) === sinkA.name));
    const res = await record(Recorder, {
      enableMicrophone: false, enableSystemAudio: true, encoding: true, audioProcessing: { backend: 'none' },
      audioDevices: { speaker: sinkA.name },
    }, { durationMs: 2400, actions: [{ at: 800, run: () => def.set(sinkB.name) }] });
    assert.equal(res.stopResult, true);
    const logs = res.events.log.map((l) => l.message);
    assert.ok(!logs.some((m) => m.startsWith('System audio source: monitor of PlaudSwitchB')), logs.join('\n'));
    const pcm = decodeOpus(writeOgg(res.ogg, 'pinned-default.ogg'));
    const p500 = tonePower(pcm, 500, 1.4 * sr, 2.2 * sr);
    const p1500 = tonePower(pcm, 1500, 1.4 * sr, 2.2 * sr);
    assert.ok(p500 > 100 * p1500, `stayed on the selected output: 500 Hz ${p500}, 1500 Hz ${p1500}`);
  } finally {
    def.restore();
  }
});

test('setAudioDevice() switches a running recording to another output', { skip }, async () => {
  const res = await record(Recorder, {
    enableMicrophone: false, enableSystemAudio: true, encoding: true, audioProcessing: { backend: 'none' },
    audioDevices: { speaker: sinkA.name },
  }, {
    durationMs: 3000,
    actions: [{
      at: 1200,
      run: (r) => {
        const target = Recorder.listSpeakerDevicesDetailed().find((d) => d.uid === sinkB.name);
        assert.deepEqual(r.setAudioDevice({ microphone: null, speaker: sinkB.name }),
          { microphone: null, speaker: target.id });
      },
    }],
  });
  assert.equal(res.stopResult, true);
  assert.deepEqual(res.events.fatal, []);
  const pcm = decodeOpus(writeOgg(res.ogg, 'set-device.ogg'));
  assert.ok(tonePower(pcm, 500, 0.3 * sr, 1.1 * sr) > 100 * tonePower(pcm, 1500, 0.3 * sr, 1.1 * sr));
  assert.ok(tonePower(pcm, 1500, 1.8 * sr, 2.8 * sr) > 100 * tonePower(pcm, 500, 1.8 * sr, 2.8 * sr));
});

test('muting the captured output records silence until unmuted', { skip }, async () => {
  try {
    const res = await record(Recorder, {
      enableMicrophone: false, enableSystemAudio: true, encoding: true, audioProcessing: { backend: 'none' },
      audioDevices: { speaker: sinkA.name },
    }, {
      durationMs: 3000,
      actions: [
        { at: 1000, run: () => pactl('set-sink-mute', sinkA.name, '1') },
        { at: 2000, run: () => pactl('set-sink-mute', sinkA.name, '0') },
      ],
    });
    assert.equal(res.stopResult, true);
    const muted = res.events.audio.filter((e) => e.at >= 1250 && e.at < 1950);
    assert.ok(muted.length > 40 && muted.every((e) => e.systemVolume === 0), 'silent while muted');
    const live = res.events.audio.filter((e) => e.at >= 2300 && e.at < 2900);
    assert.ok(live.length > 40 && live.every((e) => e.systemVolume > 0.1), 'audible after unmute');
  } finally {
    pactl('set-sink-mute', sinkA.name, '0');
  }
});

test('speaker mute monitor follows the default output and its mute flag', { skip }, async () => {
  const def = overrideDefault('sink', sinkA.name);
  pactl('set-sink-mute', sinkB.name, '1');
  const events = [];
  const statuses = () => events.filter((e) => e.type === 'status').map((e) => e.obj.muted);
  const messages = () => events.filter((e) => e.type === 'log').map((e) => e.obj.message);
  try {
    assert.ok(await waitFor(() => defaultUid(Recorder.listSpeakerDevicesDetailed()) === sinkA.name));
    const idA = Recorder.listSpeakerDevicesDetailed().find((d) => d.uid === sinkA.name).id;
    const idB = Recorder.listSpeakerDevicesDetailed().find((d) => d.uid === sinkB.name).id;
    Recorder.startSpeakerMuteMonitor((e) => events.push(e));
    assert.ok(await waitFor(() => statuses().length === 1));
    assert.deepEqual(statuses(), [false]);
    assert.deepEqual(messages(), [`Added mute listener to device ${idA}`, 'LinuxSpeakerMuteMonitor started.']);

    pactl('set-sink-mute', sinkA.name, '1');
    assert.ok(await waitFor(() => statuses().at(-1) === true), JSON.stringify(events));
    assert.equal(Recorder.isSpeakerMuted(), true);
    assert.ok(messages().includes(`Mute state changed on device ${idA}: muted`));
    pactl('set-sink-mute', sinkA.name, '0');
    assert.ok(await waitFor(() => statuses().at(-1) === false), JSON.stringify(events));
    assert.equal(Recorder.isSpeakerMuted(), false);
    assert.ok(messages().includes(`Mute state changed on device ${idA}: unmuted`));

    def.set(sinkB.name);
    assert.ok(await waitFor(() => statuses().at(-1) === true), JSON.stringify(events));
    assert.deepEqual(statuses(), [false, true, false, true]);
    assert.equal(Recorder.isSpeakerMuted(), true);
    const log = messages();
    assert.ok(log.includes(`Removed mute listener from device ${idA}`), log.join('\n'));
    assert.ok(log.includes('Default output device changed.'));
    assert.ok(log.includes(`Added mute listener to device ${idB}`));
    assert.ok(log.includes(`Mute state changed on device ${idA}: muted`));
  } finally {
    Recorder.stopSpeakerMuteMonitor();
    pactl('set-sink-mute', sinkB.name, '0');
    def.restore();
  }
});
