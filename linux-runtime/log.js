'use strict';

// Logger for the Linux runtime. Writes next to the official app's logs
// ($XDG_CONFIG_HOME/Plaud/logs) so a single directory holds everything needed
// to diagnose a problem. Warnings and errors are mirrored to stderr.

const fs = require('node:fs');
const path = require('node:path');
const paths = require('./paths');

const LEVELS = { debug: 10, info: 20, warn: 30, error: 40 };
const threshold = LEVELS[process.env.PLAUD_LINUX_LOG_LEVEL] ?? LEVELS.info;

function serialize(value) {
  if (value instanceof Error) return { name: value.name, message: value.message, code: value.code, stack: value.stack };
  return value;
}

function format(level, scope, message, data) {
  const extra = data === undefined ? '' : ` ${JSON.stringify(data, (_k, v) => serialize(v))}`;
  return `${new Date().toISOString()} [${level}] [linux:${scope}] ${message}${extra}\n`;
}

function logFile() {
  const day = new Date().toISOString().slice(0, 10);
  return path.join(paths.plaudUserData(), 'logs', `linux-runtime-${day}.log`);
}

function append(file, line) {
  try {
    fs.appendFileSync(file, line);
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.appendFileSync(file, line);
  }
}

function write(level, scope, message, data) {
  if (LEVELS[level] < threshold) return;
  const line = format(level, scope, message, data);
  try {
    append(logFile(), line);
  } catch {
    // Logging must never break the app; stderr below still carries the line.
  }
  if (LEVELS[level] >= LEVELS.warn || process.env.PLAUD_LINUX_LOG_STDERR === '1') process.stderr.write(line);
}

/** Returns a logger bound to `scope`. */
function createLogger(scope) {
  return {
    debug: (message, data) => write('debug', scope, message, data),
    info: (message, data) => write('info', scope, message, data),
    warn: (message, data) => write('warn', scope, message, data),
    error: (message, data) => write('error', scope, message, data),
  };
}

module.exports = { createLogger, logFile };
