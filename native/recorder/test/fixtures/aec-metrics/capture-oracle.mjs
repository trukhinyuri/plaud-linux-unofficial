// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

// Regenerates expected.json from a reference Recorder addon. The fixture in
// the repo was captured on macOS (arm64) from the official Plaud Desktop
// 1.4.1 recorder_mac.node with:
//
//   node capture-oracle.mjs /Applications/Plaud.app/Contents/Resources/recorder_mac.node expected.json
//
// Only Recorder.computeAecLightMetrics is called (a pure function); the
// addon is loaded from the user's own installation and nothing from it is
// stored except the returned numbers and error strings.

import { createRequire } from 'node:module';
import { writeFileSync } from 'node:fs';
import { CASES, PROBES, sceneHashes, caseArgs, runCall, encodeNumbers } from './vectors.mjs';

// Result objects all share one key order; it is stored once and each result
// becomes an array of values in that order.
function compact(outcome, state) {
  if (outcome.kind !== 'result') return outcome;
  const keys = Object.keys(outcome.value);
  if (!state.keys) state.keys = keys;
  if (keys.join() !== state.keys.join()) throw new Error('result key order differs between calls');
  return { ...outcome, value: keys.map((k) => outcome.value[k]) };
}

function main(addonPath, outPath) {
  const require = createRequire(import.meta.url);
  const { Recorder } = require(addonPath);
  const fn = (...a) => Recorder.computeAecLightMetrics(...a);
  const state = {};
  const scenes = {};
  const cases = {};
  for (const c of CASES) {
    const [id, sceneName, optName, form] = c;
    if (!scenes[sceneName]) scenes[sceneName] = sceneHashes(sceneName);
    cases[id] = { scene: sceneName, options: optName, form, outcome: compact(runCall(fn, caseArgs(c)), state) };
  }
  const probes = {};
  for (const [id, build] of Object.entries(PROBES)) probes[id] = compact(runCall(fn, build()), state);
  const enc = (v) => JSON.stringify(v, encodeNumbers);
  const section = (name, obj, last) =>
    `${enc(name)}: {\n${Object.entries(obj).map(([k, v]) => `  ${enc(k)}: ${enc(v)}`).join(',\n')}\n}${last ? '' : ','}`;
  const head = {
    about: 'Expected Recorder.computeAecLightMetrics outputs for vectors.mjs, captured from the official macOS addon (see capture-oracle.mjs).',
    capturedWith: { node: process.version, platform: process.platform, arch: process.arch, functionLength: Recorder.computeAecLightMetrics.length },
    keys: state.keys,
  };
  const lines = ['{', ...Object.entries(head).map(([k, v]) => `${enc(k)}: ${enc(v)},`)];
  lines.push(section('scenes', scenes), section('cases', cases), section('probes', probes, true), '}');
  writeFileSync(outPath, lines.join('\n') + '\n');
}

// The node test runner may execute every .mjs below test/ when started
// without arguments; this file is a tool, not a test.
if (!process.env.NODE_TEST_CONTEXT) {
  const [addonPath, outPath] = process.argv.slice(2);
  if (!addonPath || !outPath) {
    console.error('usage: node capture-oracle.mjs <recorder addon .node> <out.json>');
    process.exit(2);
  }
  main(addonPath, outPath);
}
