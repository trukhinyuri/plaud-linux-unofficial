// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Runs one AudioMonitor the way Plaud's Electron main process does and
// speaks JSON lines: commands on stdin, replies and every callback event on
// stdout. Usage: node monitor-host.mjs <path to audio_monitor_linux.node>
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { createInterface } from 'node:readline';

const require = createRequire(import.meta.url);
const { AudioMonitor } = require(process.argv[2]);
const monitor = new AudioMonitor();
const children = [];

const write = (msg) => process.stdout.write(`${JSON.stringify({ t: Date.now(), ...msg })}\n`);
const forward = (channel) => (ev) => write({ kind: 'event', channel, ev });

const commands = {
  start: () => monitor.start(forward('main')),
  stop: () => monitor.stop(),
  startMic: () => monitor.startMicMonitor(forward('mic')),
  stopMic: () => monitor.stopMicMonitor(),
  startSpeaker: () => monitor.startSpeakerMonitor(forward('speaker')),
  stopSpeaker: () => monitor.stopSpeakerMonitor(),
  // A capture process that is a descendant of the monitor's process.
  spawn: ({ argv }) => {
    const child = spawn(argv[0], argv.slice(1), { stdio: 'ignore' });
    children.push(child);
    return child.pid;
  },
  killChildren: () => {
    for (const c of children) c.kill('SIGTERM');
    children.length = 0;
  },
  exit: () => {
    for (const c of children) c.kill('SIGTERM');
    monitor.stopMicMonitor();
    monitor.stopSpeakerMonitor();
    monitor.stop();
    setTimeout(() => process.exit(0), 200);
  },
};

createInterface({ input: process.stdin }).on('line', (line) => {
  let msg;
  try {
    msg = JSON.parse(line);
  } catch {
    return;
  }
  const handler = commands[msg.cmd];
  if (!handler) {
    write({ kind: 'reply', id: msg.id, cmd: msg.cmd, error: `unknown command ${msg.cmd}` });
    return;
  }
  try {
    const value = handler(msg);
    write({ kind: 'reply', id: msg.id, cmd: msg.cmd, value: value === undefined ? null : value });
  } catch (err) {
    write({ kind: 'reply', id: msg.id, cmd: msg.cmd, error: err.message, errorName: err.constructor.name });
  }
});
process.stdin.on('end', () => commands.exit());
