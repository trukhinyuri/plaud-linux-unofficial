// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// The module must expose exactly what Plaud's main process uses from
// audio_monitor_mac.node / audio_monitor_win.node:
//   new AudioMonitor(); start(cb); stop(); startMicMonitor(cb) -> uid;
//   stopMicMonitor(); startSpeakerMonitor(cb) -> uid; stopSpeakerMonitor().
import assert from 'node:assert/strict';
import { readdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { test } from 'node:test';
import { addonPath } from './helpers.mjs';

const require = createRequire(import.meta.url);
const threadCount = () => readdirSync('/proc/self/task').length;

test('module exports the AudioMonitor class with every member', () => {
  const mod = require(addonPath);
  assert.deepEqual(Object.keys(mod), ['AudioMonitor']);
  const { AudioMonitor } = mod;
  assert.equal(typeof AudioMonitor, 'function');
  for (const name of ['start', 'stop', 'startMicMonitor', 'stopMicMonitor', 'startSpeakerMonitor', 'stopSpeakerMonitor']) {
    assert.equal(typeof AudioMonitor.prototype[name], 'function', `AudioMonitor.prototype.${name}`);
  }
  const m = new AudioMonitor();
  assert.ok(m instanceof AudioMonitor);
});

test('loading and constructing starts no thread and no connection', () => {
  const before = threadCount();
  const { AudioMonitor } = require(addonPath);
  const monitors = [new AudioMonitor(), new AudioMonitor()];
  assert.equal(threadCount(), before);
  assert.equal(monitors.length, 2);
});

test('callbacks are validated like the official addons', () => {
  const { AudioMonitor } = require(addonPath);
  const m = new AudioMonitor();
  for (const method of ['start', 'startMicMonitor', 'startSpeakerMonitor']) {
    assert.throws(() => m[method](), { name: 'TypeError', message: 'A callback function is required.' }, method);
    assert.throws(() => m[method]('nope'), { name: 'TypeError', message: 'A callback function is required.' }, method);
  }
});

test('stop methods are no-ops when nothing runs', () => {
  const { AudioMonitor } = require(addonPath);
  const m = new AudioMonitor();
  assert.equal(m.stop(), undefined);
  assert.equal(m.stopMicMonitor(), undefined);
  assert.equal(m.stopSpeakerMonitor(), undefined);
});

test('start twice throws, stop releases the PipeWire thread', async () => {
  const { AudioMonitor } = require(addonPath);
  const m = new AudioMonitor();
  const before = threadCount();
  const events = [];
  m.start((ev) => events.push(ev));
  assert.ok(threadCount() > before, 'start() runs the PipeWire and detector threads');
  assert.throws(() => m.start(() => {}), { name: 'Error', message: 'Monitor has already been started.' });
  await new Promise((resolve) => setTimeout(resolve, 300));
  m.stop();
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.equal(threadCount(), before, 'stop() joins every native thread');
  const first = events[0];
  assert.equal(first.type, 'log');
  assert.equal(first.obj.source, 'NODE');
  assert.equal(first.obj.level, 'info');
  assert.match(first.obj.message, /^Starting PipeWire monitor v\d+\.\d+\.\d+$/);
  assert.equal(typeof first.obj.meta, 'object');
  assert.ok(events.some((e) => e.type === 'log' && e.obj.message === 'Stopping monitor'));
  for (const e of events) {
    assert.equal(e.type, 'log');
    for (const v of Object.values(e.obj.meta)) assert.equal(typeof v, 'string');
  }
});
