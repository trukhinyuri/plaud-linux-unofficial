// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Meeting detection for native apps on a private PipeWire + WirePlumber
// instance: a virtual microphone (null sink with media.class
// Audio/Source/Virtual) and capture clients carrying a meeting app's identity.
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { after, before, test } from 'node:test';
import {
  Fixtures,
  MonitorHost,
  audioUnavailableReason,
  haveCommand,
  sleep,
  startPrivatePipeWire,
  tempDir,
  uniquePrefix,
  waitForNode,
} from './helpers.mjs';

const prefix = uniquePrefix('meet');
const source = `${prefix}src`;
const speakerBus = `${prefix}bus`;
const fx = new Fixtures();
const unavailable = audioUnavailableReason();
let server;
let host;
let dir;

const isMonitor = (app, pid, active) => (e) =>
  e.type === 'monitor' && e.obj.appName === app && e.obj.meta.pid === String(pid) && e.obj.isActive === active;

before(async () => {
  if (unavailable) return;
  server = await startPrivatePipeWire();
  dir = fx.track(tempDir('meet'));
  fx.loadModule('module-null-sink', [
    `sink_name=${source}`,
    'media.class=Audio/Source/Virtual',
    'channel_map=mono',
    'sink_properties=node.description=PlaudTestVirtualMic',
  ]);
  fx.loadModule('module-null-sink', [`sink_name=${speakerBus}`, 'sink_properties=node.description=PlaudTestBus']);
  await waitForNode(source);
  await waitForNode(speakerBus);
  host = new MonitorHost();
  await host.send('start');
  await host.waitLog((l) => l.message === 'Connected to PipeWire', 5000, 'PipeWire connection');
});

after(async () => {
  if (host) await host.close();
  await fx.cleanup();
  if (server) await server.stop();
});

test('native app: ACTIVE when it captures the microphone, INACTIVE 3 s after it stops', { skip: unavailable }, async () => {
  const rec = fx.spawn('pw-record', [
    '--target', source,
    '-P', '{ application.name="Zoom" application.process.binary="zoom" }',
    join(dir, 'zoom.wav'),
  ]);
  const active = await host.waitEvent('main', isMonitor('zoom', rec.pid, true), 8000, 'zoom ACTIVE');
  const obj = active.ev.obj;
  assert.equal(obj.notifyType, 1);
  assert.equal(obj.meta.appName, 'zoom');
  assert.equal(obj.meta.isActive, '1');
  assert.equal(obj.meta.deviceId, source);
  assert.equal(obj.meta.deviceName, 'PlaudTestVirtualMic');
  assert.equal(obj.meta.audioDirection, 'input');
  assert.ok(obj.meta.processName.length > 0);
  assert.ok(host.hasLog((l) => l.message === `Audio PID ${rec.pid} matched capture stream: zoom (via PID ${rec.pid})`));
  assert.ok(host.hasLog((l) => l.message === 'ACTIVE zoom'));

  const mark = host.mark();
  const stoppedAt = Date.now();
  await fx.kill(rec);
  const pending = await host.waitLog((l) => l.message === 'INACTIVE pending (start) zoom', 4000, 'INACTIVE pending', mark);
  assert.equal(pending.ev.obj.meta.delaySeconds, '3');
  const inactive = await host.waitEvent('main', (e) => e.type === 'monitor' && e.obj.appName === 'zoom' && !e.obj.isActive, 8000, 'zoom INACTIVE', mark);
  const elapsed = inactive.t - stoppedAt;
  assert.ok(elapsed >= 2900 && elapsed <= 5500, `INACTIVE after ${elapsed} ms`);
  assert.equal(inactive.ev.obj.meta.isActive, '0');
  assert.ok(host.hasLog((l) => l.message === 'INACTIVE zoom'));
});

const noParecord = !haveCommand('parecord') && 'parecord not installed';
test('pulse client: identified from application.name and application.process.id', { skip: unavailable || noParecord }, async () => {
  const rec = fx.spawn('parecord', ['--device', source, '--client-name=Zoom', join(dir, 'pulse.wav')]);
  const active = await host.waitEvent('main', isMonitor('zoom', rec.pid, true), 8000, 'zoom ACTIVE via pipewire-pulse');
  assert.equal(active.ev.obj.meta.deviceId, source);
  const mark = host.mark();
  await fx.kill(rec);
  await host.waitEvent('main', (e) => e.type === 'monitor' && e.obj.appName === 'zoom' && !e.obj.isActive, 8000, 'zoom INACTIVE', mark);
});

test('capture of an output (system audio), not a microphone, is not a meeting', { skip: unavailable }, async () => {
  const rec = fx.spawn('pw-record', [
    '--target', speakerBus,
    '-P', '{ application.name="Slack" application.process.binary="slack" stream.capture.sink=true }',
    join(dir, 'monitor.wav'),
  ]);
  await sleep(3000);
  assert.ok(!host.events('main').some((e) => e.type === 'monitor' && e.obj.meta.pid === String(rec.pid)));
  assert.ok(!host.hasLog((l) => l.message.startsWith(`Audio PID ${rec.pid} matched`)));
  await fx.kill(rec);
});

test('the monitor process and its descendants are never reported', { skip: unavailable }, async () => {
  const { value: pid } = await host.send('spawn', {
    argv: ['pw-record', '--target', source, '-P', '{ application.process.binary="discord" }', join(dir, 'self.wav')],
  });
  await sleep(3000);
  assert.ok(!host.events('main').some((e) => e.type === 'monitor' && e.obj.meta.pid === String(pid)));
  assert.ok(!host.hasLog((l) => l.message.startsWith(`Audio PID ${pid} matched`)));
  await host.send('killChildren');
});

test('re-joining within the grace period cancels INACTIVE and is a new session', { skip: unavailable }, async () => {
  const first = fx.spawn('pw-record', ['--target', source, '-P', '{ application.process.binary="slack" }', join(dir, 's1.wav')]);
  await host.waitEvent('main', isMonitor('slack', first.pid, true), 8000, 'slack ACTIVE');
  await sleep(2100); // beyond the 2 s per-app notification throttle
  const mark = host.mark();
  await fx.kill(first);
  await host.waitLog((l) => l.message === 'INACTIVE pending (start) slack', 4000, 'INACTIVE pending', mark);
  const second = fx.spawn('pw-record', ['--target', source, '-P', '{ application.process.binary="slack" }', join(dir, 's2.wav')]);
  await host.waitLog((l) => l.message === 'CANCEL INACTIVE slack', 4000, 'CANCEL INACTIVE', mark);
  await host.waitLog((l) => l.message === 'COUNTER RESET -> NEW SESSION slack', 4000, 'new session', mark);
  await host.waitEvent('main', isMonitor('slack', second.pid, true), 4000, 'slack re-notified', mark);
  const sinceMark = host.messages.slice(mark).filter((m) => m.kind === 'event').map((m) => m.ev);
  assert.ok(!sinceMark.some((e) => e.type === 'monitor' && e.obj.appName === 'slack' && !e.obj.isActive));
  const end = host.mark();
  await fx.kill(second);
  await host.waitEvent('main', (e) => e.type === 'monitor' && e.obj.appName === 'slack' && !e.obj.isActive, 8000, 'slack INACTIVE', end);
});
