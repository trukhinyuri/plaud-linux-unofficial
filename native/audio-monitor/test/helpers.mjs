// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the AudioMonitor integration tests: a private audio
// server, a monitor host process (the addon runs outside the test runner so
// that processes spawned by the tests are not its descendants), PipeWire
// fixtures with guaranteed cleanup, and a WAV tone generator.
import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
export const addonPath = join(here, '..', 'build', 'Release', 'audio_monitor_linux.node');
export const selftestPath = join(here, '..', 'build', 'Release', 'audio_monitor_selftest');
export const fakeChromePath = join(here, '..', 'build', 'Release', 'test-bin', 'chrome');
const hostScript = join(here, 'fixtures', 'monitor-host.mjs');

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
export const uniquePrefix = (tag) => `plaud-test-${process.pid}-${tag}-`;

export function tempDir(tag) {
  return mkdtempSync(join(tmpdir(), `plaud-am-${tag}-`));
}

export function run(cmd, args, options = {}) {
  return execFileSync(cmd, args, { encoding: 'utf8', timeout: 10000, stdio: ['ignore', 'pipe', 'pipe'], ...options });
}

// --- the addon in a separate process -------------------------------------------

export class MonitorHost {
  constructor(env = {}) {
    this.messages = [];
    this.waiters = new Set();
    this.child = spawn(process.execPath, [hostScript, addonPath], {
      stdio: ['pipe', 'pipe', 'inherit'],
      env: { ...process.env, ...env },
    });
    this.exited = once(this.child, 'exit');
    createInterface({ input: this.child.stdout }).on('line', (line) => {
      let msg;
      try {
        msg = JSON.parse(line);
      } catch {
        msg = { kind: 'stdout', line };
      }
      this.messages.push(msg);
      for (const w of [...this.waiters]) w.check(msg);
    });
  }

  get pid() {
    return this.child.pid;
  }

  // Sends a command and resolves with its reply.
  async send(cmd, extra = {}) {
    const id = `${cmd}-${Math.random().toString(36).slice(2)}`;
    const reply = this.waitFor((m) => m.kind === 'reply' && m.id === id, 15000, `reply to ${cmd}`);
    this.child.stdin.write(`${JSON.stringify({ cmd, id, ...extra })}\n`);
    const r = await reply;
    if (r.error) throw new Error(`${cmd}: ${r.error}`);
    return r;
  }

  events(channel) {
    return this.messages.filter((m) => m.kind === 'event' && (!channel || m.channel === channel)).map((m) => m.ev);
  }

  logs() {
    return this.events('main').filter((e) => e.type === 'log').map((e) => e.obj);
  }

  hasLog(predicate) {
    return this.logs().some(predicate);
  }

  // Resolves with the first message (already received or future) matching
  // `predicate`; `since` limits the search to messages after that index.
  waitFor(predicate, timeoutMs, label, since = 0) {
    const existing = this.messages.slice(since).find(predicate);
    if (existing) return Promise.resolve(existing);
    return new Promise((resolve, reject) => {
      const waiter = {
        check: (msg) => {
          if (!predicate(msg)) return;
          clearTimeout(timer);
          this.waiters.delete(waiter);
          resolve(msg);
        },
      };
      const timer = setTimeout(() => {
        this.waiters.delete(waiter);
        const tail = this.messages.slice(-25).map((m) => JSON.stringify(m)).join('\n');
        reject(new Error(`timeout after ${timeoutMs} ms waiting for ${label}\nlast messages:\n${tail}`));
      }, timeoutMs);
      this.waiters.add(waiter);
    });
  }

  waitEvent(channel, predicate, timeoutMs, label, since = 0) {
    return this.waitFor((m) => m.kind === 'event' && m.channel === channel && predicate(m.ev), timeoutMs, label, since);
  }

  waitLog(predicate, timeoutMs, label, since = 0) {
    return this.waitEvent('main', (e) => e.type === 'log' && predicate(e.obj), timeoutMs, label, since);
  }

  mark() {
    return this.messages.length;
  }

  async close() {
    if (this.child.exitCode !== null || this.child.signalCode !== null) return;
    try {
      this.child.stdin.write(`${JSON.stringify({ cmd: 'exit', id: 'exit' })}\n`);
    } catch {
      // stdin already closed
    }
    const timer = setTimeout(() => this.child.kill('SIGKILL'), 8000);
    await this.exited;
    clearTimeout(timer);
  }
}

// --- processes and PipeWire objects owned by a test ----------------------------

export class Fixtures {
  constructor() {
    this.children = [];
    this.modules = [];
    this.paths = [];
  }

  spawn(cmd, args, options = {}) {
    const child = spawn(cmd, args, { stdio: ['ignore', 'ignore', 'pipe'], ...options });
    child.stderrText = '';
    child.stderr?.on('data', (d) => {
      child.stderrText += d.toString();
    });
    child.exitedPromise = once(child, 'exit');
    this.children.push(child);
    return child;
  }

  async kill(child, signal = 'SIGTERM') {
    if (child.exitCode !== null || child.signalCode !== null) return;
    child.kill(signal);
    const timer = setTimeout(() => child.kill('SIGKILL'), 3000);
    await child.exitedPromise;
    clearTimeout(timer);
  }

  loadModule(name, args) {
    const id = run('pactl', ['load-module', name, ...args]).trim();
    this.modules.push(id);
    return id;
  }

  unloadModule(id) {
    try {
      run('pactl', ['unload-module', id]);
    } catch {
      // already gone
    }
    this.modules = this.modules.filter((m) => m !== id);
  }

  track(path) {
    this.paths.push(path);
    return path;
  }

  async cleanup() {
    for (const child of [...this.children].reverse()) await this.kill(child);
    this.children = [];
    for (const id of [...this.modules].reverse()) this.unloadModule(id);
    for (const p of this.paths) rmSync(p, { recursive: true, force: true });
    this.paths = [];
  }
}

// --- PipeWire graph queries ------------------------------------------------------

export function pwNodes() {
  const dump = JSON.parse(run('pw-dump', [], { maxBuffer: 64 * 1024 * 1024 }));
  return dump.filter((o) => o.type === 'PipeWire:Interface:Node').map((o) => ({ id: o.id, state: o.info?.state, props: o.info?.props ?? {} }));
}

export async function waitForNode(name, timeoutMs = 5000, env = undefined) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const out = run('pw-dump', [], { maxBuffer: 64 * 1024 * 1024, env });
    const node = JSON.parse(out).find((o) => o.type === 'PipeWire:Interface:Node' && o.info?.props?.['node.name'] === name);
    if (node) return node;
    await sleep(100);
  }
  throw new Error(`PipeWire node ${name} did not appear within ${timeoutMs} ms`);
}

// Waits until no node named `name` (and matching `where(props)`) exists.
export async function waitForNodeGone(name, timeoutMs = 5000, where = () => true) {
  const deadline = Date.now() + timeoutMs;
  const matches = (n) => n.props['node.name'] === name && where(n.props);
  while (Date.now() < deadline) {
    if (!pwNodes().some(matches)) return;
    await sleep(100);
  }
  throw new Error(`PipeWire node ${name} still present after ${timeoutMs} ms`);
}

// --- private audio server ----------------------------------------------------------
//
// The integration tests run against their own PipeWire + WirePlumber +
// pipewire-pulse in a temporary runtime directory, without hardware monitors:
// they never see the machine's devices, never change its defaults and leave
// nothing in the user's WirePlumber state. This process and every child
// (pactl, pw-*, the monitor host) reach it through the environment.

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

const AUDIO_TOOLS = ['pipewire', 'wireplumber', 'pipewire-pulse', 'pactl', 'pw-cli', 'pw-dump', 'pw-play', 'pw-record', 'pw-loopback', 'pw-link'];

// Why the integration tests cannot run here (a node:test `skip` value), or false.
export function audioUnavailableReason() {
  if (!existsSync(addonPath)) return 'addon not built (npm run build)';
  for (const cmd of AUDIO_TOOLS) {
    if (!haveCommand(cmd)) return `${cmd} not installed`;
  }
  return false;
}

async function until(check, what, timeoutMs = 8000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      if (check()) return;
    } catch {
      // not up yet
    }
    await sleep(25);
  }
  throw new Error(`private PipeWire: ${what} did not come up within ${timeoutMs} ms`);
}

export async function startPrivatePipeWire() {
  const root = tempDir('pw');
  const runtime = join(root, 'runtime');
  mkdirSync(runtime, { mode: 0o700 });
  const confDir = join(root, 'config', 'wireplumber', 'wireplumber.conf.d');
  mkdirSync(confDir, { recursive: true });
  writeFileSync(join(confDir, '90-plaud-audio-monitor-tests.conf'), WIREPLUMBER_TEST_CONF);
  Object.assign(process.env, {
    PIPEWIRE_RUNTIME_DIR: runtime,
    XDG_RUNTIME_DIR: runtime,
    XDG_CONFIG_HOME: join(root, 'config'),
    XDG_STATE_HOME: join(root, 'state'),
    PULSE_RUNTIME_PATH: join(runtime, 'pulse'),
    PULSE_SERVER: `unix:${join(runtime, 'pulse', 'native')}`,
  });
  delete process.env.PIPEWIRE_REMOTE;
  delete process.env.PIPEWIRE_CORE;
  const pdeathsig = haveCommand('setpriv');
  const daemons = [];
  const launch = (cmd) => {
    // With setpriv the daemons also die when the test process is killed.
    const child = pdeathsig
      ? spawn('setpriv', ['--pdeathsig', 'TERM', '--', cmd], { stdio: 'ignore' })
      : spawn(cmd, [], { stdio: 'ignore' });
    child.exitedPromise = once(child, 'exit');
    daemons.push(child);
  };
  const stop = async () => {
    const running = daemons.filter((d) => d.exitCode === null && d.signalCode === null);
    for (const d of running) d.kill('SIGTERM');
    const timer = setTimeout(() => running.forEach((d) => d.kill('SIGKILL')), 3000);
    await Promise.all(running.map((d) => d.exitedPromise));
    clearTimeout(timer);
    rmSync(root, { recursive: true, force: true });
  };
  try {
    launch('pipewire');
    await until(() => existsSync(join(runtime, 'pipewire-0')), 'pipewire');
    launch('wireplumber');
    launch('pipewire-pulse');
    await until(() => run('pw-cli', ['ls', 'Metadata']).includes('metadata.name = "default"'), 'wireplumber');
    await until(() => run('pactl', ['info']).length > 0, 'pipewire-pulse');
  } catch (err) {
    await stop();
    throw err;
  }
  return { root, runtime, stop };
}

// --- audio -----------------------------------------------------------------------

// 16-bit mono PCM WAV with a sine tone.
export function writeToneWav(path, { seconds = 20, freq = 1000, amplitude = 0.5, rate = 48000 } = {}) {
  const frames = Math.round(seconds * rate);
  const data = Buffer.alloc(frames * 2);
  for (let i = 0; i < frames; i++) {
    data.writeInt16LE(Math.round(amplitude * 32767 * Math.sin((2 * Math.PI * freq * i) / rate)), i * 2);
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
  writeFileSync(path, Buffer.concat([header, data]));
  return path;
}

export function haveCommand(cmd) {
  try {
    run('sh', ['-c', `command -v ${cmd}`]);
    return true;
  } catch {
    return false;
  }
}

// Sets the private server's default source or sink and waits until the
// session manager has applied it.
export async function setDefault(kind, name, timeoutMs = 3000) {
  run('pactl', [`set-default-${kind}`, name]);
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (run('pactl', [`get-default-${kind}`]).trim() === name) return;
    await sleep(25);
  }
  throw new Error(`default ${kind} did not become ${name} within ${timeoutMs} ms`);
}
