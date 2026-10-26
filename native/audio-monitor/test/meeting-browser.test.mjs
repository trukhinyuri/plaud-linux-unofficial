// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Browser meetings need WebRTC evidence (the macOS addon requires an active
// RTCPeerConnection): an unconnected UDP socket in the browser's process
// tree, or, secondarily, a browser window showing a meeting service.
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { createInterface } from 'node:readline';
import { after, before, test } from 'node:test';
import {
  Fixtures,
  MonitorHost,
  audioUnavailableReason,
  fakeChromePath,
  sleep,
  startPrivatePipeWire,
  tempDir,
  uniquePrefix,
  waitForNode,
} from './helpers.mjs';

const prefix = uniquePrefix('web');
const source = `${prefix}src`;
const fx = new Fixtures();
const unavailable = audioUnavailableReason();
let server;
let host;
let dir;
let windowsFile;

// test/native/fake_chrome.c: an executable named "chrome" whose child process
// captures the microphone with Chrome's stream identity.
async function startBrowser(mode) {
  const child = fx.spawn(fakeChromePath, [mode, source, join(dir, `${mode}-${Date.now()}.wav`)], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const [line] = await once(createInterface({ input: child.stdout }), 'line');
  return { child, ...JSON.parse(line) };
}

before(async () => {
  if (unavailable) return;
  server = await startPrivatePipeWire();
  dir = fx.track(tempDir('web'));
  windowsFile = join(dir, 'windows.json');
  writeFileSync(windowsFile, '[]');
  fx.loadModule('module-null-sink', [
    `sink_name=${source}`,
    'media.class=Audio/Source/Virtual',
    'channel_map=mono',
    'sink_properties=node.description=PlaudTestWebMic',
  ]);
  await waitForNode(source);
  host = new MonitorHost({ PLAUD_AUDIO_MONITOR_WINDOW_LIST: windowsFile });
  await host.send('start');
  await host.waitLog((l) => l.message === 'Connected to PipeWire', 5000, 'PipeWire connection');
});

after(async () => {
  if (host) await host.close();
  await fx.cleanup();
  if (server) await server.stop();
});

test('browser in a WebRTC call: ACTIVE after 2.5 s of stable UDP evidence', { skip: unavailable }, async () => {
  const b = await startBrowser('udp');
  const matched = await host.waitLog(
    (l) => l.message === `Audio PID ${b.recPid} matched capture stream: chrome (via PID ${b.browserPid})`,
    8000,
    'chrome capture matched',
  );
  const pending = await host.waitLog((l) => l.message === 'ACTIVE pending (start) chrome', 4000, 'ACTIVE pending');
  assert.equal(pending.ev.obj.meta.stableMs, '2500');
  const active = await host.waitEvent('main', (e) => e.type === 'monitor' && e.obj.appName === 'chrome' && e.obj.isActive, 8000, 'chrome ACTIVE');
  assert.ok(active.t - pending.t >= 2400, `stable for ${active.t - pending.t} ms`);
  assert.ok(active.t - matched.t >= 2400);
  const meta = active.ev.obj.meta;
  assert.equal(meta.evidence, 'webrtc-udp');
  assert.ok(meta.udpSocket.startsWith(`${b.browserPid}:0.0.0.0:`), meta.udpSocket);
  assert.equal(meta.browserEngine, 'chromium');
  assert.equal(meta.pid, String(b.recPid));
  assert.equal(meta.deviceId, source);
  const mark = host.mark();
  await fx.kill(b.child);
  await host.waitEvent('main', (e) => e.type === 'monitor' && e.obj.appName === 'chrome' && !e.obj.isActive, 8000, 'chrome INACTIVE', mark);
});

test('browser using the microphone without WebRTC evidence is not a meeting', { skip: unavailable }, async () => {
  const mark = host.mark();
  const b = await startBrowser('none');
  await host.waitLog(
    (l) => l.message === `Audio PID ${b.recPid} ignored (browser without WebRTC evidence): chrome` && l.level === 'debug',
    8000,
    'ignored browser capture',
    mark,
  );
  await sleep(3500);
  const events = host.messages.slice(mark).filter((m) => m.kind === 'event').map((m) => m.ev);
  assert.ok(!events.some((e) => e.type === 'monitor' && e.obj.appName === 'chrome'));
  assert.ok(!events.some((e) => e.type === 'log' && e.obj.message === 'ACTIVE pending (start) chrome'));
  await fx.kill(b.child);
});

test('a browser window showing a meeting is secondary evidence', { skip: unavailable }, async () => {
  const mark = host.mark();
  const b = await startBrowser('none');
  writeFileSync(windowsFile, JSON.stringify([
    { pid: b.browserPid, appId: 'google-chrome', title: 'Meet - abc-defg-hij - Google Chrome' },
    { pid: 1, appId: 'other', title: 'Zoom Meeting' },
  ]));
  const active = await host.waitEvent('main', (e) => e.type === 'monitor' && e.obj.appName === 'chrome' && e.obj.isActive, 10000, 'chrome ACTIVE by title', mark);
  assert.equal(active.ev.obj.meta.evidence, 'window-title');
  assert.equal(active.ev.obj.meta.meetingService, 'google-meet');
  assert.ok(host.hasLog((l) => l.message === 'Window title provider: file'));
  const end = host.mark();
  await fx.kill(b.child);
  writeFileSync(windowsFile, '[]');
  await host.waitEvent('main', (e) => e.type === 'monitor' && e.obj.appName === 'chrome' && !e.obj.isActive, 8000, 'chrome INACTIVE', end);
});
