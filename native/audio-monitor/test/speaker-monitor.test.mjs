// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Smart speaker selection on a private PipeWire + WirePlumber instance. Two
// fake hardware outputs (pw-loopback sinks with device.api=alsa whose output
// goes to a silent null sink) are the only physical outputs; playing a tone
// into one of them makes it the active output.
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { after, before, test } from 'node:test';
import {
  Fixtures,
  MonitorHost,
  audioUnavailableReason,
  pwNodes,
  setDefault,
  sleep,
  startPrivatePipeWire,
  tempDir,
  uniquePrefix,
  waitForNode,
  waitForNodeGone,
  writeToneWav,
} from './helpers.mjs';

const prefix = uniquePrefix('spk');
const hole = `${prefix}hole`;
const spkA = `${prefix}devA`;
const spkB = `${prefix}devB`;
const descA = 'Plaud Test Speaker A';
const descB = 'Plaud Test Speaker B';
const fx = new Fixtures();
const unavailable = audioUnavailableReason();
const loopbacks = new Map();
let server;
let host;
let tone;

async function addFakeSpeaker(name, description) {
  const child = fx.spawn('pw-loopback', [
    '--name', `${name}-loopback`,
    '--capture-props', `{ node.name=${name} node.description="${description}" media.class=Audio/Sink device.api=alsa device.bus=usb node.virtual=false }`,
    '--playback-props', `{ node.name=${name}-out target.object=${hole} }`,
  ]);
  loopbacks.set(name, child);
  await waitForNode(name);
}

async function removeFakeSpeaker(name) {
  await fx.kill(loopbacks.get(name));
  loopbacks.delete(name);
  await waitForNodeGone(name);
}

function playTone(target) {
  return fx.spawn('pw-play', ['--target', target, tone]);
}

const speakerEvent = (type, deviceId) => (e) =>
  e.type === 'speakerEvent' && e.speakerEventType === type && (deviceId === undefined || e.deviceId === deviceId);
const speakerEventsSince = (mark) => host.messages.slice(mark).filter((m) => m.kind === 'event' && m.channel === 'speaker');

before(async () => {
  if (unavailable) return;
  server = await startPrivatePipeWire();
  const dir = fx.track(tempDir('spk'));
  tone = writeToneWav(join(dir, 'tone.wav'), { seconds: 20 });
  fx.loadModule('module-null-sink', [`sink_name=${hole}`, 'sink_properties=node.description=PlaudTestSilence']);
  await waitForNode(hole);
  await addFakeSpeaker(spkA, descA);
  await addFakeSpeaker(spkB, descB);
  // The default output is a virtual sink, which smart selection never uses.
  await setDefault('sink', hole);
  host = new MonitorHost();
  await host.send('start');
  await host.waitLog((l) => l.message === 'Connected to PipeWire', 5000, 'PipeWire connection');
});

after(async () => {
  if (host) await host.close();
  await fx.cleanup();
  if (server) await server.stop();
});

test('smart speaker: initial device, follows the playing output, fallback, no speaker', { skip: unavailable }, async () => {
  const { value: initial } = await host.send('startSpeaker');
  assert.equal(initial, spkA, 'first physical output when the default sink is virtual');
  await host.waitLog((l) => l.message === `[SpeakerMonitor] Initial device by fallback: ${descA}`, 2000, 'initial log');
  const meters = pwNodes().filter((n) => n.props['node.name'] === 'plaud-speaker-meter');
  assert.deepEqual(meters.map((n) => n.props['media.name']).sort(), [descA, descB], 'one passive meter per physical output');

  // Sound on B while A is silent: two consecutive 500 ms evaluations switch.
  let mark = host.mark();
  const playerB = playTone(spkB);
  const toB = await host.waitEvent('speaker', speakerEvent('speakerAutoSelected', spkB), 6000, 'switch to B', mark);
  assert.deepEqual(Object.keys(toB.ev).sort(), ['deviceId', 'deviceName', 'speakerEventType', 'type']);
  assert.equal(toB.ev.deviceName, descB);
  await host.waitLog(
    (l) => l.message.startsWith(`[SpeakerMonitor] Switch \u2192 ${descB} uid=${spkB}, target peak=`) && l.message.includes('(current peak='),
    2000,
    'switch log',
  );
  await fx.kill(playerB);

  // Sound moves to A; the playing current output is kept while B plays too.
  mark = host.mark();
  const playerA = playTone(spkA);
  await host.waitEvent('speaker', speakerEvent('speakerAutoSelected', spkA), 6000, 'switch to A', mark);
  const both = playTone(spkB);
  const keep = host.mark();
  await sleep(2000);
  assert.deepEqual(speakerEventsSince(keep), [], 'no switch away from a playing output');
  await fx.kill(both);
  await fx.kill(playerA);

  // A restart picks the system default output when it is a physical one.
  await host.send('stopSpeaker');
  await host.waitLog((l) => l.message === '[SpeakerMonitor] Stopped', 3000, 'speaker monitor stopped');
  await waitForNodeGone('plaud-speaker-meter', 3000);
  await setDefault('sink', spkB);
  await sleep(300);
  mark = host.mark();
  const { value: byDefault } = await host.send('startSpeaker');
  assert.equal(byDefault, spkB);
  await host.waitLog(
    (l) => l.message === `[SpeakerMonitor] Initial device by system default: ${descB} (peak=0.0000)`,
    2000,
    'initial by default log',
    mark,
  );

  // The selected output disappears, then the last one.
  mark = host.mark();
  await removeFakeSpeaker(spkB);
  const fallback = await host.waitEvent('speaker', speakerEvent('speakerAutoSelected', spkA), 5000, 'fallback to A', mark);
  assert.equal(fallback.ev.deviceName, descA);
  await host.waitLog((l) => l.message === `[SpeakerMonitor] Device lost, fallback: ${descA}`, 2000, 'fallback log', mark);

  mark = host.mark();
  await removeFakeSpeaker(spkA);
  const none = await host.waitEvent('speaker', speakerEvent('noSpeaker'), 5000, 'noSpeaker', mark);
  assert.equal(none.ev.deviceId, '');
  assert.equal(none.ev.deviceName, '');
  await host.waitLog((l) => l.message === '[SpeakerMonitor] No render device available' && l.level === 'warn', 2000, 'no device log', mark);

  await host.send('stopSpeaker');
  await host.waitLog((l) => l.message === '[SpeakerMonitor] Stopped', 3000, 'speaker monitor stopped', mark);
});
