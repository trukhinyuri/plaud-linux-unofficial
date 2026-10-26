#!/usr/bin/env node
'use strict';

// Builds the two Linux native addons and describes their JavaScript API.
//
//   node scripts/build-native.js                          build both addons
//   node scripts/build-native.js --test [--fail-on-skip]  build, then run their test suites
//
// Each addon is a self-contained npm package under native/<name>/ whose
// `npm run build` produces build/Release/<addon>.node. The audio tests skip
// themselves on machines without the PipeWire tools they need; with
// --fail-on-skip (CI) a skipped test is an error.

const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const { REPO_ROOT, log, parseArgs, run } = require('./lib/common');

const ADDONS = [
  { dir: 'native/recorder', file: 'recorder_linux.node', className: 'Recorder' },
  { dir: 'native/audio-monitor', file: 'audio_monitor_linux.node', className: 'AudioMonitor' },
];

const addonPath = (addon) => path.join(REPO_ROOT, addon.dir, 'build', 'Release', addon.file);

function buildAddon(addon) {
  const cwd = path.join(REPO_ROOT, addon.dir);
  if (!fs.existsSync(path.join(cwd, 'package.json'))) throw new Error(`${addon.dir}/package.json not found`);
  const hasLock = fs.existsSync(path.join(cwd, 'package-lock.json'));
  log('native', `${addon.dir}: npm ${hasLock ? 'ci' : 'install'}`);
  run('npm', [hasLock ? 'ci' : 'install', '--no-audit', '--no-fund'], { cwd });
  log('native', `${addon.dir}: npm run build`);
  run('npm', ['run', 'build'], { cwd });
  if (!fs.existsSync(addonPath(addon))) throw new Error(`build did not produce ${path.relative(REPO_ROOT, addonPath(addon))}`);
}

/**
 * Loads each addon in a separate Node process and lists the exported classes'
 * static and prototype members. Loading an addon must not start any audio
 * work (confirmed for both addons), so this is side-effect free.
 */
function describeNativeApi() {
  const script = `
    const out = { classes: {}, modules: {} };
    for (const [file, className] of JSON.parse(process.argv[1])) {
      const mod = require(file);
      out.modules[require('node:path').basename(file)] = Object.keys(mod).sort();
      const cls = mod[className];
      if (typeof cls !== 'function') throw new Error(file + ' does not export class ' + className);
      const own = (o) => Object.getOwnPropertyNames(o).filter((n) => typeof Object.getOwnPropertyDescriptor(o, n).value === 'function' && n !== 'constructor');
      out.classes[className] = { statics: own(cls).sort(), prototype: own(cls.prototype).sort() };
    }
    process.stdout.write(JSON.stringify(out));
  `;
  const res = spawnSync(process.execPath, ['-e', script, JSON.stringify(ADDONS.map((a) => [addonPath(a), a.className]))], {
    encoding: 'utf8',
    timeout: 60_000,
  });
  if (res.status !== 0) throw new Error(`loading the native addons failed:\n${res.stderr}`);
  return JSON.parse(res.stdout);
}

/**
 * Number of skipped tests in the summary node:test prints ("ℹ skipped N" from
 * the spec reporter, "# skipped N" or the older "# skip N" from TAP), or null
 * when there is no summary.
 */
function skippedCount(output) {
  const plain = output.replace(/\x1b\[[0-9;]*m/g, ''); // without ANSI colours
  const counts = [...plain.matchAll(/^(?:ℹ|#) skip(?:ped)? (\d+)$/gm)].map((m) => Number(m[1]));
  return counts.length ? counts.reduce((a, b) => a + b, 0) : null;
}

/** Runs `npm test` in `cwd`, passing its output through; resolves with the standard output. */
function npmTest(cwd) {
  return new Promise((resolve, reject) => {
    const child = spawn('npm', ['test'], { cwd, stdio: ['ignore', 'pipe', 'inherit'] });
    let out = '';
    child.stdout.on('data', (chunk) => {
      out += chunk;
      process.stdout.write(chunk);
    });
    child.on('error', (error) => reject(new Error(`failed to run npm test: ${error.message}`)));
    child.on('close', (code, signal) => {
      if (code === 0) resolve(out);
      else reject(new Error(`npm test in ${path.relative(REPO_ROOT, cwd)} exited with status ${code ?? signal}`));
    });
  });
}

async function testAddons({ failOnSkip }) {
  const skipped = [];
  for (const addon of ADDONS) {
    log('native', `${addon.dir}: npm test`);
    const out = await npmTest(path.join(REPO_ROOT, addon.dir));
    const count = skippedCount(out);
    if (count === null && failOnSkip) throw new Error(`${addon.dir}: no node:test summary in the test output`);
    if (count > 0) skipped.push(`${addon.dir}: ${count}`);
  }
  if (skipped.length) {
    const message = `skipped tests (${skipped.join(', ')})`;
    if (failOnSkip) throw new Error(`${message}; install the tools the skip reasons name`);
    log('native', message);
  }
}

module.exports = { ADDONS, addonPath, buildAddon, describeNativeApi, skippedCount };

async function main() {
  const args = parseArgs(process.argv.slice(2), { booleans: ['test', 'fail-on-skip'] });
  for (const addon of ADDONS) buildAddon(addon);
  const api = describeNativeApi();
  for (const [name, cls] of Object.entries(api.classes)) {
    log('native', `${name}: ${cls.statics.length} static, ${cls.prototype.length} prototype methods`);
  }
  if (args.test) await testAddons({ failOnSkip: Boolean(args['fail-on-skip']) });
}

if (require.main === module) {
  main().catch((error) => {
    process.stderr.write(`build-native: ${error.message}\n`);
    process.exitCode = 1;
  });
}
