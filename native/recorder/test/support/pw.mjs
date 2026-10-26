// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// Shared helpers for the recorder tests: a private, hardware-less PipeWire +
// WirePlumber + pipewire-pulse instance (so tests never touch the session's
// real devices, defaults or WirePlumber state), virtual devices created with
// pactl, WAV generation, playback with pw-play, recording, Ogg parsing and
// decoding. Importing this module has no side effects besides an exit hook,
// so the test runner may execute it as a (trivially passing) file.

import { execFileSync, spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
export const moduleRoot = path.resolve(here, '..', '..');
export const SAMPLE_RATE = 48000;

export function addonPath() {
  return process.env.RECORDER_ADDON
    ? path.resolve(process.env.RECORDER_ADDON)
    : path.join(moduleRoot, 'build', 'Release', 'recorder_linux.node');
}

export function loadAddon() {
  return createRequire(import.meta.url)(addonPath());
}

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export function hasCommand(cmd) {
  return spawnSync('sh', ['-c', `command -v ${cmd}`], { stdio: 'ignore' }).status === 0;
}

const AUDIO_TOOLS = ['pipewire', 'wireplumber', 'pipewire-pulse', 'pactl', 'pw-play', 'pw-cli', 'ffmpeg', 'ffprobe'];

/** Why the audio tests cannot run here, or false when they can (usable as node:test `skip`). */
export function audioUnavailableReason() {
  for (const cmd of AUDIO_TOOLS) {
    if (!hasCommand(cmd)) return `${cmd} not installed`;
  }
  return false;
}

const cleanups = [];
export function onCleanup(fn) {
  cleanups.push(fn);
}
// A recorder, watcher or child process left running (a failed test that
// never reached its stop()) keeps the test process alive, and node --test
// then waits for that file forever. Once a file's tests are done, it gets
// this long to exit before it fails, naming what is still open.
const LINGER_MS = 30000;

export function runCleanups() {
  while (cleanups.length > 0) {
    const fn = cleanups.pop();
    try {
      fn();
    } catch {
      // Best effort: keep tearing down the rest.
    }
  }
  setTimeout(() => {
    console.error(`test process still alive ${LINGER_MS} ms after its tests ended; open handles: `
      + `${process.getActiveResourcesInfo().join(', ')}`);
    process.exit(1);
  }, LINGER_MS).unref();
}
process.on('exit', runCleanups);

// WirePlumber for tests: no hardware monitors, only the virtual devices the
// tests create. WirePlumber 0.5 reads the profiles from
// wireplumber.conf.d/*.conf. WirePlumber 0.4 (Ubuntu 22.04/24.04, Debian 12)
// ignores that section: its monitors are switched by Lua fragments that it
// merges by file name across its config directories, so the switches are set
// between the 50-*-config.lua defaults and 90-enable-all.lua. Monitors that an
// older 0.4 release lacks are skipped.
const WIREPLUMBER_TEST_CONF = `wireplumber.profiles = {
  main = {
    monitor.alsa = disabled
    monitor.alsa-midi = disabled
    monitor.bluez = disabled
    monitor.bluez-midi = disabled
    monitor.v4l2 = disabled
    monitor.libcamera = disabled
  }
}
`;
const luaDisable = (...monitors) => monitors.map((m) => `if ${m} then ${m}.enabled = false end\n`).join('');
const WIREPLUMBER_04_TEST_LUA = {
  'main.lua.d': luaDisable('alsa_monitor', 'v4l2_monitor', 'libcamera_monitor'),
  'bluetooth.lua.d': luaDisable('bluez_monitor', 'bluez_midi_monitor'),
};

/**
 * Starts a private PipeWire instance (pipewire, wireplumber, pipewire-pulse)
 * in a temporary runtime directory and points this process and every child
 * (pactl, pw-play, the addon) at it. Returns { start, stop, crash,
 * waitReady } to simulate daemon loss and restart; everything is torn down
 * on cleanup.
 */
export async function startPrivatePipeWire() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'plaud-rec-pw-'));
  const runtime = path.join(root, 'runtime');
  fs.mkdirSync(runtime, { mode: 0o700 });
  const wpConfig = path.join(root, 'config', 'wireplumber');
  fs.mkdirSync(path.join(wpConfig, 'wireplumber.conf.d'), { recursive: true });
  fs.writeFileSync(path.join(wpConfig, 'wireplumber.conf.d', '90-plaud-recorder-tests.conf'), WIREPLUMBER_TEST_CONF);
  for (const [dir, lua] of Object.entries(WIREPLUMBER_04_TEST_LUA)) {
    fs.mkdirSync(path.join(wpConfig, dir));
    fs.writeFileSync(path.join(wpConfig, dir, '51-plaud-recorder-tests.lua'), lua);
  }
  Object.assign(process.env, {
    PIPEWIRE_RUNTIME_DIR: runtime,
    XDG_RUNTIME_DIR: runtime,
    XDG_CONFIG_HOME: path.join(root, 'config'),
    XDG_STATE_HOME: path.join(root, 'state'),
    PULSE_RUNTIME_PATH: path.join(runtime, 'pulse'),
    PULSE_SERVER: `unix:${path.join(runtime, 'pulse', 'native')}`,
  });
  delete process.env.PIPEWIRE_REMOTE;
  // The daemons die with the test process even if it is killed.
  const pdeathsig = hasCommand('setpriv') ? 'setpriv --pdeathsig TERM -- ' : '';
  let daemons = [];
  const launch = (cmd, delaySec = 0) => {
    const script = delaySec > 0 ? `sleep ${delaySec.toFixed(3)}; exec ${pdeathsig}${cmd}` : `exec ${pdeathsig}${cmd}`;
    daemons.push(spawn('sh', ['-c', script], { stdio: 'ignore' }));
  };
  const ready = (check, what) => waitFor(check, 8000, 25).then((ok) => {
    if (!ok) throw new Error(`private PipeWire: ${what} did not come up`);
  });
  const pipewireUp = () => spawnSync('pw-cli', ['info', '0'], { stdio: 'ignore' }).status === 0;
  const waitReady = async () => {
    await ready(pipewireUp, 'pipewire');
    await ready(() => (spawnSync('pw-cli', ['ls', 'Metadata'], { encoding: 'utf8' }).stdout ?? '')
      .includes('metadata.name = "default"'), 'wireplumber');
    await ready(() => spawnSync('pactl', ['info'], { stdio: 'ignore' }).status === 0, 'pipewire-pulse');
  };
  const start = async () => {
    launch('pipewire');
    await ready(pipewireUp, 'pipewire');
    launch('wireplumber');
    launch('pipewire-pulse');
    await waitReady();
  };
  const stop = async () => {
    const running = daemons.filter((d) => d.exitCode === null && d.signalCode === null);
    daemons = [];
    await Promise.all(running.map((d) => new Promise((resolve) => {
      d.once('exit', resolve);
      d.kill('SIGTERM');
    })));
    for (const f of fs.readdirSync(runtime)) fs.rmSync(path.join(runtime, f), { recursive: true, force: true });
  };
  /**
   * Kills the daemons with SIGKILL, synchronously, and optionally schedules
   * their restart from a shell after `restartAfterMs`, so the restart happens
   * even while the JS thread is blocked (e.g. in a synchronous restart()).
   */
  const crash = (restartAfterMs) => {
    for (const d of daemons) if (d.exitCode === null && d.signalCode === null) d.kill('SIGKILL');
    daemons = [];
    if (restartAfterMs !== undefined) {
      launch('pipewire', restartAfterMs / 1000);
      launch('wireplumber', restartAfterMs / 1000 + 0.3);
      launch('pipewire-pulse', restartAfterMs / 1000 + 0.3);
    }
  };
  onCleanup(() => {
    for (const d of daemons) if (d.exitCode === null) d.kill('SIGTERM');
    fs.rmSync(root, { recursive: true, force: true });
  });
  await start();
  return { root, runtime, start, stop, crash, waitReady };
}

export function pactl(...args) {
  return execFileSync('pactl', args, { encoding: 'utf8' }).trim();
}

function unloadModule(id) {
  spawnSync('pactl', ['unload-module', String(id)], { stdio: 'ignore' });
}

let uniq = 0;
export function uniqueName(prefix) {
  uniq += 1;
  return `${prefix}_${process.pid}_${uniq}`;
}

/**
 * A virtual output (Audio/Sink). Whatever plays into it is its monitor.
 * `extraProps` are additional node properties ("key=value key2=value2").
 */
export function createNullSink(name, description = name, extraProps = '') {
  const props = `device.description=${description}${extraProps ? ` ${extraProps}` : ''}`;
  const id = pactl('load-module', 'module-null-sink', `sink_name=${name}`, `sink_properties="${props}"`,
    'rate=48000', 'channels=2');
  const handle = { id, name, unloaded: false };
  handle.unload = () => {
    if (!handle.unloaded) {
      handle.unloaded = true;
      unloadModule(id);
    }
  };
  onCleanup(handle.unload);
  return handle;
}

/**
 * An acoustic-echo stand-in: loops the monitor of output `sinkName` into the
 * sink `feedName` (a virtual microphone's feed) with `delaySec` of delay.
 * Resolves once both ends are linked.
 */
export async function createEchoPath(sinkName, feedName, delaySec = 0.02) {
  const name = uniqueName('plaud_test_echo');
  const pin = 'node.dont-fallback = true node.dont-move = true node.dont-reconnect = true';
  const child = spawn('pw-loopback', ['-n', name, '-c', '1', '-m', '[ MONO ]', '--delay', String(delaySec),
    '--capture-props', `{ stream.capture.sink = true target.object = ${sinkName} ${pin} }`,
    '--playback-props', `{ target.object = ${feedName} ${pin} }`], { stdio: 'ignore' });
  onCleanup(() => {
    if (child.exitCode === null) child.kill('SIGTERM');
  });
  const linked = await waitFor(() => {
    const out = spawnSync('pw-link', ['-l'], { encoding: 'utf8' }).stdout ?? '';
    return out.includes(`${sinkName}:monitor_`) && out.includes(`${feedName}:playback_`)
      && out.split('\n').filter((l) => l.includes(name)).length >= 2;
  });
  if (!linked) throw new Error(`echo path ${sinkName} -> ${feedName} did not link`);
  return child;
}

/**
 * A virtual microphone: a mono null sink `<name>_feed` plus a remap source
 * `<name>` (Audio/Source) reading the feed's monitor. Play into `handle.feed`
 * and the audio comes out of the microphone `handle.name`.
 */
export function createVirtualMic(name, description = name) {
  const feed = `${name}_feed`;
  const feedId = pactl('load-module', 'module-null-sink', `sink_name=${feed}`,
    `sink_properties=device.description=${description}_feed`, 'channel_map=mono', 'rate=48000');
  let micId;
  try {
    micId = pactl('load-module', 'module-remap-source', `master=${feed}.monitor`, `source_name=${name}`,
      `source_properties=device.description=${description}`, 'channel_map=mono');
  } catch (e) {
    unloadModule(feedId);
    throw e;
  }
  const handle = { id: micId, feedId, name, feed, unloaded: false };
  handle.unload = () => {
    if (!handle.unloaded) {
      handle.unloaded = true;
      unloadModule(micId);
      unloadModule(feedId);
    }
  };
  onCleanup(handle.unload);
  return handle;
}

export async function waitFor(predicate, timeoutMs = 4000, stepMs = 50) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const value = await predicate();
    if (value) return value;
    if (Date.now() > deadline) return value;
    await sleep(stepMs);
  }
}

/** Temporarily makes `name` the default sink/source; restored on cleanup. */
export function overrideDefault(kind, name) {
  const getter = kind === 'sink' ? 'get-default-sink' : 'get-default-source';
  const setter = kind === 'sink' ? 'set-default-sink' : 'set-default-source';
  const original = pactl(getter);
  pactl(setter, name);
  let restored = false;
  const restore = () => {
    if (!restored) {
      restored = true;
      spawnSync('pactl', [setter, original], { stdio: 'ignore' });
    }
  };
  onCleanup(restore);
  return { original, set: (n) => pactl(setter, n), restore };
}

export function tmpFile(name) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'plaud-rec-test-'));
  onCleanup(() => fs.rmSync(dir, { recursive: true, force: true }));
  return path.join(dir, name);
}

/** 16-bit PCM WAV. */
export function writeWav(file, samples, rate = SAMPLE_RATE) {
  const data = Buffer.alloc(samples.length * 2);
  for (let i = 0; i < samples.length; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    data.writeInt16LE(Math.round(s * 32767), i * 2);
  }
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + data.length, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22);
  header.writeUInt32LE(rate, 24);
  header.writeUInt32LE(rate * 2, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([header, data]));
  return file;
}

export function tone(freq, seconds, amp = 0.3, rate = SAMPLE_RATE) {
  const n = Math.round(seconds * rate);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) out[i] = amp * Math.sin((2 * Math.PI * freq * i) / rate);
  return out;
}

/** Deterministic band-limited noise (speech-like spectrum, good for AEC). */
export function noise(seconds, amp = 0.2, seed = 1, rate = SAMPLE_RATE) {
  const n = Math.round(seconds * rate);
  const out = new Float32Array(n);
  let state = seed >>> 0 || 1;
  let lp = 0;
  for (let i = 0; i < n; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    const white = state / 2147483648 - 1;
    lp = 0.75 * lp + 0.25 * white;
    out[i] = amp * 2.2 * lp;
  }
  return out;
}

function sinkExists(name) {
  const out = spawnSync('pactl', ['list', 'short', 'sinks'], { encoding: 'utf8' }).stdout ?? '';
  return out.split('\n').some((line) => line.split('\t')[1] === name);
}

/**
 * Plays a WAV file into the virtual sink `target`. The stream may never fall
 * back to (or be moved to) another output, so a missing target stays silent
 * instead of playing through the machine's speakers.
 */
export function play(target, file) {
  if (!sinkExists(target)) throw new Error(`play(): ${target} is not a sink`);
  const child = spawn('pw-play', ['--target', target,
    '-P', '{ node.dont-fallback = true node.dont-reconnect = true node.dont-move = true }', file],
  { stdio: 'ignore' });
  onCleanup(() => {
    if (child.exitCode === null) child.kill('SIGTERM');
  });
  return child;
}

export function stopPlayback(child) {
  if (child.exitCode === null) child.kill('SIGTERM');
}

/**
 * Runs a recording and collects every callback event. The recorder is stopped
 * even when an action throws: a running recorder's callback keeps the test
 * process alive, so a failed test would otherwise hang the test run.
 */
export async function record(Recorder, options, { durationMs, actions = [] } = {}) {
  const recorder = new Recorder();
  const events = { log: [], audio: [], encoded: [], apm: [], fatal: [], other: [], order: [] };
  const started = Date.now();
  const startResult = recorder.start(options, (ev) => {
    const bucket = events[ev.type] ?? events.other;
    bucket.push(ev.type === 'audio' || ev.type === 'apm' || ev.type === 'encoded'
      ? { ...ev, at: Date.now() - started }
      : ev);
    events.order.push(ev.type);
  });
  let stopResult;
  try {
    const pending = [...actions].sort((a, b) => a.at - b.at);
    for (const action of pending) {
      const wait = action.at - (Date.now() - started);
      if (wait > 0) await sleep(wait);
      await action.run(recorder, events);
    }
    const remaining = durationMs - (Date.now() - started);
    if (remaining > 0) await sleep(remaining);
  } finally {
    stopResult = recorder.stop();
  }
  const ogg = Buffer.concat(events.encoded.map((e) => Buffer.from(e.data)));
  return { recorder, startResult, stopResult, events, ogg };
}

export function writeOgg(buf, name = 'out.ogg') {
  const file = tmpFile(name);
  fs.writeFileSync(file, buf);
  return file;
}

export function probe(file) {
  const out = execFileSync('ffprobe', ['-v', 'error', '-show_streams', '-show_format', '-of', 'json', file], {
    encoding: 'utf8',
  });
  return JSON.parse(out);
}

/** Decodes the Opus stream of a recording to mono float PCM at 48 kHz. */
export function decodeOpus(file) {
  const out = execFileSync('ffmpeg', ['-v', 'error', '-i', file, '-map', '0:a:0', '-f', 'f32le', '-ac', '1',
    '-ar', String(SAMPLE_RATE), 'pipe:1'], { maxBuffer: 1 << 30 });
  return new Float32Array(out.buffer, out.byteOffset, Math.floor(out.length / 4));
}

export function rms(x, start = 0, end = x.length) {
  let acc = 0;
  const s = Math.max(0, Math.floor(start));
  const e = Math.min(x.length, Math.floor(end));
  for (let i = s; i < e; i++) acc += x[i] * x[i];
  return e > s ? Math.sqrt(acc / (e - s)) : 0;
}

/** Normalized power of one frequency in x[start, end) (Goertzel). */
export function tonePower(x, freq, start = 0, end = x.length, rate = SAMPLE_RATE) {
  const s = Math.max(0, Math.floor(start));
  const e = Math.min(x.length, Math.floor(end));
  const w = (2 * Math.PI * freq) / rate;
  const coeff = 2 * Math.cos(w);
  let s1 = 0;
  let s2 = 0;
  for (let i = s; i < e; i++) {
    const s0 = x[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  const n = e - s;
  return n > 0 ? power / (n * n) : 0;
}

export function db(ratio) {
  return 10 * Math.log10(ratio);
}

/** Ogg pages: { flags, granule (BigInt), serial, seq, segments, body, packets }. */
export function parseOggPages(buf) {
  const pages = [];
  let off = 0;
  while (off + 27 <= buf.length) {
    if (buf.toString('latin1', off, off + 4) !== 'OggS') throw new Error(`no OggS capture pattern at ${off}`);
    const flags = buf[off + 5];
    const granule = buf.readBigInt64LE(off + 6);
    const serial = buf.readUInt32LE(off + 14);
    const seq = buf.readUInt32LE(off + 18);
    const nseg = buf[off + 26];
    const segments = [...buf.subarray(off + 27, off + 27 + nseg)];
    const bodyLen = segments.reduce((a, b) => a + b, 0);
    const bodyStart = off + 27 + nseg;
    const body = buf.subarray(bodyStart, bodyStart + bodyLen);
    const packets = [];
    let cur = 0;
    let len = 0;
    for (const s of segments) {
      len += s;
      if (s < 255) {
        packets.push(body.subarray(cur, cur + len));
        cur += len;
        len = 0;
      }
    }
    pages.push({ offset: off, flags, granule, serial, seq, segments, body, packets });
    off = bodyStart + bodyLen;
  }
  if (off !== buf.length) throw new Error(`trailing ${buf.length - off} bytes after the last Ogg page`);
  return pages;
}
