// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Smart microphone selection on a private PipeWire + WirePlumber instance.
// Two fake hardware microphones (pw-loopback sources with device.api=alsa,
// each fed from its own null-sink bus) are the only input devices; a tone
// played into a bus is "voice" on that microphone.
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { after, before, test } from 'node:test';
import {
  Fixtures,
  MonitorHost,
  audioUnavailableReason,
  pwNodes,
  setDefault,
  startPrivatePipeWire,
  tempDir,
  uniquePrefix,
  waitForNode,
  waitForNodeGone,
  writeToneWav,
} from './helpers.mjs';

const prefix = uniquePrefix('mic');
const busA = `${prefix}busA`;
const busB = `${prefix}busB`;
const micA = `${prefix}devA`;
const micB = `${prefix}devB`;
const descA = 'Plaud Test Mic A';
const descB = 'Plaud Test Mic B';
const fx = new Fixtures();
const unavailable = audioUnavailableReason();
const loopbacks = new Map();
let server;
let host;
let tone;

async function addFakeMic(name, description, bus) {
  const child = fx.spawn('pw-loopback', [
    '--name', `${name}-loopback`,
    '--channels', '1',
    '--capture-props', `{ node.name=${name}-capture target.object=${bus} stream.capture.sink=true audio.position=[ MONO ] }`,
    '--playback-props', `{ node.name=${name} node.description="${description}" media.class=Audio/Source device.api=alsa device.bus=usb node.virtual=false audio.position=[ MONO ] }`,
  ]);
  loopbacks.set(name, child);
  await waitForNode(name);
}

async function removeFakeMic(name) {
  await fx.kill(loopbacks.get(name));
  loopbacks.delete(name);
  await waitForNodeGone(name);
}

function playTone(bus) {
  return fx.spawn('pw-play', ['--target', bus, tone]);
}

const micEvent = (type, deviceId) => (e) => e.type === 'micEvent' && e.micEventType === type && (deviceId === undefined || e.deviceId === deviceId);

before(async () => {
  if (unavailable) return;
  server = await startPrivatePipeWire();
  const dir = fx.track(tempDir('mic'));
  tone = writeToneWav(join(dir, 'tone.wav'), { seconds: 30 });
  fx.loadModule('module-null-sink', [`sink_name=${busA}`, 'channel_map=mono', 'sink_properties=node.description=PlaudTestBusA']);
  fx.loadModule('module-null-sink', [`sink_name=${busB}`, 'channel_map=mono', 'sink_properties=node.description=PlaudTestBusB']);
  await waitForNode(busA);
  await waitForNode(busB);
  await addFakeMic(micA, descA, busA);
  await addFakeMic(micB, descB, busB);
  // B is the system default although A was plugged in first.
  await setDefault('source', micB);
  host = new MonitorHost();
  await host.send('start');
  await host.waitLog((l) => l.message === 'Connected to PipeWire', 5000, 'PipeWire connection');
});

after(async () => {
  if (host) await host.close();
  await fx.cleanup();
  if (server) await server.stop();
});

test('smart microphone: system default first, voice-driven switch, fallback, weak voice, no device', { skip: unavailable }, async () => {
  const { value: initial } = await host.send('startMic');
  assert.equal(initial, micB, 'the system default microphone is the initial selection');
  await host.waitLog((l) => l.message === `Initial device: ${descB} uid=${micB}`, 2000, 'initial device log');
  for (const [name, desc] of [[micA, descA], [micB, descB]]) {
    await host.waitLog(
      (l) => l.message === `[MicMonitor] Capture started: ${desc} uid=${name} fmt=48000Hz/1ch/32bit/float/interleaved`,
      5000,
      `capture of ${name}`,
    );
  }
  const { value: again } = await host.send('startMic');
  assert.equal(again, micB, 'a second start returns the current selection');

  // Voice on A: 5 winning evaluations (200 ms apart) switch to A.
  let mark = host.mark();
  const playerA = playTone(busA);
  const switched = await host.waitEvent('mic', micEvent('deviceAutoSelected', micA), 8000, 'switch to A', mark);
  assert.deepEqual(Object.keys(switched.ev).sort(), ['deviceId', 'deviceName', 'micEventType', 'type']);
  assert.equal(switched.ev.deviceName, descA);
  await host.waitLog((l) => l.message === `[MicMonitor] Switch \u2192 ${descA} uid=${micA}`, 2000, 'switch log');
  await fx.kill(playerA);

  // The selected microphone is unplugged: fall back to the remaining one.
  mark = host.mark();
  const silenceFrom = Date.now();
  await removeFakeMic(micA);
  const fallback = await host.waitEvent('mic', micEvent('deviceFallbackRequired', micB), 5000, 'fallback to B', mark);
  assert.equal(fallback.ev.deviceName, descB);
  await host.waitLog((l) => l.message === `[MicMonitor] Device lost, fallback to: ${descB}`, 2000, 'fallback log');
  await host.waitLog((l) => l.message === `[MicMonitor] Device removed: ${descA}`, 2000, 'removal log');

  // 10 s without voice on every microphone.
  const weak = await host.waitEvent('mic', micEvent('weakVoice'), 16000, 'weak voice', mark);
  assert.equal(weak.ev.deviceId, '');
  assert.equal(weak.ev.deviceName, '');
  const weakAfter = weak.t - silenceFrom;
  assert.ok(weakAfter >= 9500, `weakVoice after ${weakAfter} ms of silence`);
  await host.waitLog((l) => l.message === '[MicMonitor] Weak voice on ALL devices for 10s' && l.level === 'warn', 2000, 'weak log');

  mark = host.mark();
  const playerB = playTone(busB);
  const recovered = await host.waitEvent('mic', micEvent('weakVoiceRecovered', micB), 5000, 'voice recovered', mark);
  assert.equal(recovered.ev.deviceName, descB);
  await host.waitLog((l) => l.message === `[MicMonitor] Voice recovered on ${descB}`, 2000, 'recovered log');
  await fx.kill(playerB);

  // No physical microphone left, then one is plugged in again.
  mark = host.mark();
  await removeFakeMic(micB);
  const none = await host.waitEvent('mic', micEvent('noDevice'), 5000, 'noDevice', mark);
  assert.equal(none.ev.deviceId, '');
  await host.waitLog((l) => l.message === '[MicMonitor] No physical input device available' && l.level === 'warn', 2000, 'no device log');
  mark = host.mark();
  await addFakeMic(micB, descB, busB);
  await host.waitEvent('mic', micEvent('deviceFallbackRequired', micB), 5000, 'device back', mark);
  await host.waitLog((l) => l.message === `[MicMonitor] Input device available, fallback to: ${descB}`, 2000, 'device back log');
  assert.equal(host.events('mic').filter(micEvent('noDevice')).length, 1, 'noDevice is reported once per loss');

  const meters = pwNodes().filter((n) => n.props['node.name'] === 'plaud-mic-meter');
  assert.deepEqual(meters.map((n) => n.props['media.name']), [descB], 'one meter stream per physical microphone');
  await host.send('stopMic');
  await host.waitLog((l) => l.message === '[MicMonitor] Stopped', 3000, 'mic monitor stopped');
  await waitForNodeGone('plaud-mic-meter', 3000);
});
