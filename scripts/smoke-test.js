#!/usr/bin/env node
'use strict';

// Starts the built (or an installed) app, waits until the official main
// process reports both native addons as loaded, then quits it through the
// main-process inspector (app.quit(), the same path as the tray's Quit item)
// and checks that it exited cleanly.
//
//   node scripts/smoke-test.js [--launcher <path>] [--timeout <seconds>]
//
// Requirements: a graphical session (Wayland, X11 or Xvfb), a running
// PipeWire with at least one audio device (the official app refuses to start
// without one), and no other running Plaud instance (single-instance lock).

const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const common = require('./lib/common');

const { BUILD_DIR, constants, log } = common;
const EXPECTED = ['Recorder loaded successfully from main process', 'AudioMonitor loaded'];
const FAILURES = [/\[recorder\] failed to load native module/, /\[monitor\] failed to load native module/];

function logsDir() {
  const config = process.env.XDG_CONFIG_HOME || path.join(require('node:os').homedir(), '.config');
  return path.join(config, 'Plaud', 'logs');
}

/** Reads what was appended to *.log files in `dir` since `offsets` was taken. */
class LogTail {
  constructor(dir) {
    this.dir = dir;
    this.offsets = new Map();
    for (const file of this.files()) this.offsets.set(file, fs.statSync(file).size);
    this.text = '';
  }

  files() {
    try {
      return fs.readdirSync(this.dir).filter((f) => f.endsWith('.log')).map((f) => path.join(this.dir, f));
    } catch {
      return [];
    }
  }

  poll() {
    for (const file of this.files()) {
      const size = fs.statSync(file).size;
      const from = this.offsets.get(file) ?? 0;
      if (size <= from) continue;
      const fd = fs.openSync(file, 'r');
      try {
        const buf = Buffer.alloc(size - from);
        fs.readSync(fd, buf, 0, buf.length, from);
        this.text += buf.toString('utf8');
      } finally {
        fs.closeSync(fd);
      }
      this.offsets.set(file, size);
    }
    return this.text;
  }
}

function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close(() => resolve(port));
    });
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Evaluates `expression` in the main process through the inspector protocol. */
async function inspectorEvaluate(port, expression) {
  const res = await fetch(`http://127.0.0.1:${port}/json/list`);
  const [target] = await res.json();
  if (!target?.webSocketDebuggerUrl) throw new Error('main-process inspector target not found');
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    ws.onopen = resolve;
    ws.onerror = () => reject(new Error('inspector connection failed'));
  });
  const reply = new Promise((resolve) => {
    ws.onmessage = (event) => {
      const msg = JSON.parse(event.data);
      if (msg.id === 1) resolve(msg);
    };
    ws.onclose = () => resolve(null);
  });
  ws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression } }));
  const msg = await Promise.race([reply, sleep(5000).then(() => null)]);
  try {
    ws.close();
  } catch {
    // The process may already be gone.
  }
  return msg;
}

async function smoke({ launcher, timeoutSec = 90 }) {
  if (!fs.existsSync(launcher)) throw new Error(`${launcher} not found; run "npm run build" first`);
  const tail = new LogTail(logsDir());
  const port = await freePort();
  const startedAt = new Date();
  log('smoke', `starting ${launcher}`);
  const child = spawn(launcher, [`--inspect=127.0.0.1:${port}`], {
    stdio: ['ignore', 'pipe', 'pipe'],
    env: { ...process.env, PLAUD_LINUX_NO_STAGED: '1' },
  });
  let output = '';
  child.stdout.on('data', (d) => (output += d));
  child.stderr.on('data', (d) => (output += d));
  let exit = null;
  const exited = new Promise((resolve) => child.on('exit', (code, signal) => resolve((exit = { code, signal }))));

  const deadline = Date.now() + timeoutSec * 1000;
  let text = '';
  while (Date.now() < deadline && exit === null) {
    text = tail.poll();
    if (EXPECTED.every((line) => text.includes(line))) break;
    const failure = FAILURES.find((re) => re.test(text));
    if (failure) break;
    await sleep(500);
  }
  const found = EXPECTED.filter((line) => text.includes(line));
  if (exit !== null) {
    throw new Error(
      `the app exited early (${JSON.stringify(exit)}); another Plaud instance may be running.\n--- output ---\n${output.slice(-4000)}\n--- log ---\n${text.slice(-4000)}`,
    );
  }

  // Let the UI settle before quitting (first-run windows, tray).
  await sleep(3000);
  log('smoke', 'quitting through app.quit()');
  await inspectorEvaluate(port, "process.mainModule.require('electron').app.quit()");
  const result = await Promise.race([exited, sleep(30_000).then(() => null)]);
  text = tail.poll();
  if (result === null) {
    throw new Error(`the app (pid ${child.pid}) did not quit within 30 s; it was left running for inspection`);
  }

  const problems = [];
  for (const line of EXPECTED) if (!found.includes(line)) problems.push(`log line missing: "${line}"`);
  if (result.code !== 0 || result.signal) problems.push(`unclean exit: ${JSON.stringify(result)}`);
  const coredumps = common.capture('coredumpctl', ['list', '--no-legend', '--since', startedAt.toISOString().replace('T', ' ').slice(0, 19)]);
  if (coredumps && coredumps.includes(constants.EXECUTABLE)) problems.push(`core dump recorded:\n${coredumps}`);
  if (problems.length) {
    throw new Error(`${problems.join('\n')}\n--- output ---\n${output.slice(-4000)}\n--- log ---\n${text.slice(-6000)}`);
  }
  for (const line of EXPECTED) log('smoke', `ok: "${line}"`);
  log('smoke', `ok: clean exit (${JSON.stringify(result)})`);
  return { found, exit: result };
}

module.exports = { smoke, LogTail };

if (require.main === module) {
  const args = common.parseArgs(process.argv.slice(2));
  const launcher = typeof args.launcher === 'string' ? path.resolve(args.launcher) : path.join(BUILD_DIR, `linux-${process.arch}`, 'app', constants.COMMAND);
  smoke({ launcher, timeoutSec: Number(args.timeout) || 90 }).catch((error) => {
    process.stderr.write(`smoke: ${error.message}\n`);
    process.exitCode = 1;
  });
}
