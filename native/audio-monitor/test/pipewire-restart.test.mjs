// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Recovery from an audio server crash. The monitor runs against a private,
// minimal PipeWire daemon in a temporary runtime directory, so the user's
// audio server is never touched. That daemon has no session manager: nodes,
// port layouts and links are created by hand with pw-cli and pw-link, which
// also keeps the timing of the crash and of the device re-creation exact.
import assert from 'node:assert/strict';
import { existsSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { after, before, test } from 'node:test';
import { Fixtures, MonitorHost, audioUnavailableReason, run, sleep, tempDir, uniquePrefix, waitForNode } from './helpers.mjs';

const prefix = uniquePrefix('restart');
const remote = `${prefix}pw`;
const unavailable = audioUnavailableReason();
let runtimeDir;
let serverEnv;  // the daemon's environment
let pwEnv;  // its clients' environment
const source = `${prefix}src`;
const micA = `${prefix}devA`;
const micB = `${prefix}devB`;
const speaker = `${prefix}devS`;
const fx = new Fixtures();
let dir;
let conf;
let daemon;
let host;

const pw = (cmd, args) => run(cmd, args, { env: pwEnv });
const isZoom = (pid, active) => (e) =>
  e.type === 'monitor' && e.obj.appName === 'zoom' && e.obj.meta.pid === String(pid) && e.obj.isActive === active;
const deviceEvents = (since) =>
  host.messages.slice(since).filter((m) => m.kind === 'event' && (m.channel === 'mic' || m.channel === 'speaker'));

function daemonConfig() {
  return `context.properties = {
    core.daemon = true
    core.name = ${remote}
    support.dbus = false
    mem.allow-mlock = false
    log.level = 1
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-access args = { access.socket = { ${remote} = "unrestricted", ${remote}-manager = "unrestricted" } } }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
]
context.objects = [
    { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver node.group = pipewire.dummy priority.driver = 20000 } }
    { factory = metadata args = { metadata.name = default } }
]
`;
}

function removeSocketFiles() {
  for (const name of [remote, `${remote}.lock`, `${remote}-manager`, `${remote}-manager.lock`]) {
    rmSync(join(runtimeDir, name), { force: true });
  }
}

async function startDaemon() {
  daemon = fx.spawn('pipewire', ['-c', conf], { env: serverEnv });
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (existsSync(join(runtimeDir, remote))) {
      try {
        pw('pw-cli', ['info', '0']);
        return;
      } catch {
        // socket not accepting yet
      }
    }
    await sleep(50);
  }
  throw new Error(`private PipeWire daemon did not start: ${daemon.stderrText}`);
}

async function waitForPort(port, direction) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (pw('pw-link', [direction]).split('\n').some((l) => l.trim() === port)) return;
    await sleep(50);
  }
  throw new Error(`port ${port} did not appear`);
}

function createNode(props) {
  pw('pw-cli', ['create-node', 'adapter', `{ factory.name=support.null-audio-sink object.linger=true audio.position=[ MONO ] ${props} }`]);
}

// The virtual microphone meetings capture from, with a monitor port to link.
async function createSource() {
  createNode(`node.name=${source} node.description=PlaudRestartSource media.class=Audio/Source/Virtual`);
  await waitForNode(source, 5000, pwEnv);
  pw('pw-cli', ['set-param', source, 'PortConfig',
    '{ direction: Input, mode: dsp, monitor: true, format: { mediaType: audio, mediaSubtype: raw, format: F32P, rate: 48000, channels: 1, position: [ MONO ] } }']);
  await waitForPort(`${source}:capture_MONO`, '-o');
}

// Physical-looking devices for the smart mic and speaker monitors.
async function createMic(name, description) {
  createNode(`node.name=${name} node.description="${description}" media.class=Audio/Source device.api=alsa`);
  return waitForNode(name, 5000, pwEnv);
}

async function createSpeaker() {
  createNode(`node.name=${speaker} node.description="Plaud Restart Speaker" media.class=Audio/Sink device.api=alsa`);
  return waitForNode(speaker, 5000, pwEnv);
}

// A Zoom capture client linked to the virtual microphone.
async function startZoom(tag) {
  const node = `${prefix}${tag}`;
  const rec = fx.spawn('pw-record', [
    '-P', `{ node.name=${node} application.name="Zoom" application.process.binary="zoom" }`,
    join(dir, `${tag}.wav`),
  ], { env: pwEnv });
  await waitForNode(node, 5000, pwEnv);
  pw('pw-cli', ['set-param', node, 'PortConfig',
    '{ direction: Input, mode: dsp, format: { mediaType: audio, mediaSubtype: raw, format: F32P, rate: 48000, channels: 2, position: [ FL, FR ] } }']);
  await waitForPort(`${node}:input_FL`, '-i');
  pw('pw-link', [`${source}:capture_MONO`, `${node}:input_FL`]);
  return rec;
}

before(async () => {
  if (unavailable) return;
  dir = fx.track(tempDir('restart'));
  runtimeDir = join(dir, 'runtime');
  mkdirSync(runtimeDir, { mode: 0o700 });
  const runtime = { XDG_RUNTIME_DIR: runtimeDir, PIPEWIRE_RUNTIME_DIR: runtimeDir };
  serverEnv = { ...process.env, ...runtime };
  delete serverEnv.PIPEWIRE_REMOTE;
  delete serverEnv.PIPEWIRE_CORE;
  pwEnv = { ...process.env, ...runtime, PIPEWIRE_REMOTE: remote };
  conf = join(dir, 'pipewire.conf');
  writeFileSync(conf, daemonConfig());
  await startDaemon();
  await createSource();
  await createMic(micA, 'Plaud Restart Mic A');
  await createSpeaker();
  host = new MonitorHost({ ...runtime, PIPEWIRE_REMOTE: remote });
  await host.send('start');
  await host.waitLog((l) => l.message === 'Connected to PipeWire', 5000, 'PipeWire connection');
});

after(async () => {
  if (host) await host.close();
  await fx.cleanup();
});

test('PipeWire restart: meeting ends, monitor reconnects, detection and device selection resume', { skip: unavailable }, async () => {
  const { value: initialMic } = await host.send('startMic');
  assert.equal(initialMic, micA);
  const { value: initialSpeaker } = await host.send('startSpeaker');
  assert.equal(initialSpeaker, speaker);

  const zoom1 = await startZoom('zoom1');
  await host.waitEvent('main', isZoom(zoom1.pid, true), 8000, 'ACTIVE before the crash');

  // The audio server dies: the meeting it was carrying ends after the grace period.
  let mark = host.mark();
  const killedAt = Date.now();
  await fx.kill(daemon, 'SIGKILL');
  removeSocketFiles();
  await host.waitLog((l) => l.message === 'PipeWire connection lost, reconnecting' && l.level === 'warn', 3000, 'connection lost', mark);
  const inactive = await host.waitEvent('main', isZoom(zoom1.pid, false), 8000, 'INACTIVE after the crash', mark);
  assert.ok(inactive.t - killedAt >= 2900, `INACTIVE ${inactive.t - killedAt} ms after the crash`);
  await fx.kill(zoom1);

  // The server comes back empty; the session manager re-creates the devices
  // about a second later (WirePlumber's ALSA monitor). The device selections
  // survive that window.
  await sleep(1000);
  mark = host.mark();
  await startDaemon();
  await host.waitLog((l) => l.message === 'Reconnected to PipeWire', 8000, 'reconnection', mark);
  await sleep(1000);
  await createSource();
  const nodeA = await createMic(micA, 'Plaud Restart Mic A');
  const nodeS = await createSpeaker();
  await sleep(4000);
  assert.deepEqual(deviceEvents(mark), [], 'no device change across the restart');
  const { value: stillA } = await host.send('startMic');
  assert.equal(stillA, micA);
  const { value: stillS } = await host.send('startSpeaker');
  assert.equal(stillS, speaker);

  // Detection works on the new connection.
  const zoom2 = await startZoom('zoom2');
  await host.waitEvent('main', isZoom(zoom2.pid, true), 8000, 'ACTIVE after the restart');

  // Both device monitors follow the new graph too.
  await createMic(micB, 'Plaud Restart Mic B');
  mark = host.mark();
  pw('pw-cli', ['destroy', String(nodeA.id)]);
  const fallback = await host.waitEvent(
    'mic',
    (e) => e.type === 'micEvent' && e.micEventType === 'deviceFallbackRequired',
    5000,
    'microphone fallback after the restart',
    mark,
  );
  assert.equal(fallback.ev.deviceId, micB);
  pw('pw-cli', ['destroy', String(nodeS.id)]);
  await host.waitEvent('speaker', (e) => e.type === 'speakerEvent' && e.speakerEventType === 'noSpeaker', 5000, 'noSpeaker after the restart', mark);
  await fx.kill(zoom2);
});
