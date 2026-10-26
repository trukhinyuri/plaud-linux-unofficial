'use strict';

// Native API compatibility check between an official release and the Linux
// addons of this build.
//
// The official main bundle calls into two native addons. A future release may
// call methods that our Linux addons do not implement; staging such a release
// would break recording at run time. Before accepting a release the patcher:
//  1. collects the native calls made through the well-known receivers
//     (Recorder.<static>(), recorder.<method>(), detector.<method>()) and
//     requires each to be exported by our addons;
//  2. computes "candidates": identifiers that occur both as a string in the
//     official macOS addon binaries (N-API property names are stored as C
//     strings) and as a member name in the main bundle. Candidates that are
//     new relative to the baseline release this build was verified against,
//     and that our addons do not export, refuse the release;
//  3. refuses releases that ship native addons this build does not know.
// Step 2 is deliberately conservative: an unrelated new identifier that
// happens to satisfy both conditions blocks an update rather than risking a
// broken recorder.

const path = require('node:path');

const RECEIVERS = {
  recorderStatics: /\bRecorder\.([A-Za-z_$][\w$]*)\s*\(/g,
  recorderMethods: /\brecorder\??\.([A-Za-z_$][\w$]*)\s*\(/g,
  monitorMethods: /\b(?:detector|meetingDetector|getMeetingDetector\(\))\??\.([A-Za-z_$][\w$]*)\s*\(/g,
};

// Native addons that are known and handled without a Linux counterpart:
// electron-app-universal-protocol-client loads its prebuild only on macOS.
const IGNORED_NODE_FILE = /(^|\/)node_modules\/electron-app-universal-protocol-client\/prebuilds\//;

const BINARY_IDENTIFIER = /^[a-z][A-Za-z0-9]{2,63}$/;

/** Identifier-shaped NUL/control-delimited strings found in a binary. */
function binaryIdentifiers(buf) {
  const out = new Set();
  let start = -1;
  for (let i = 0; i <= buf.length; i++) {
    const c = i < buf.length ? buf[i] : 0;
    const printable = c >= 0x20 && c < 0x7f;
    if (printable) {
      if (start === -1) start = i;
      continue;
    }
    if (start !== -1) {
      if (i - start >= 3 && i - start <= 64) {
        const s = buf.toString('latin1', start, i);
        if (BINARY_IDENTIFIER.test(s)) out.add(s);
      }
      start = -1;
    }
  }
  return out;
}

/** Member names used in JavaScript source (`.name` and `["name"]`). */
function jsMemberNames(code) {
  const out = new Set();
  for (const m of code.matchAll(/\.\s*([A-Za-z_$][\w$]*)/g)) out.add(m[1]);
  for (const m of code.matchAll(/\[\s*["']([A-Za-z_$][\w$]*)["']\s*\]/g)) out.add(m[1]);
  return out;
}

function usedNativeCalls(code) {
  const out = {};
  for (const [key, re] of Object.entries(RECEIVERS)) {
    out[key] = [...new Set([...code.matchAll(re)].map((m) => m[1]))].sort();
  }
  return out;
}

/**
 * Computes the API surface of an official release.
 * @param {{ mainSources: Map<string,string>, nativeBinaries: Map<string,Buffer>, nodeFiles: string[] }} input
 */
function computeSurface({ mainSources, nativeBinaries, nodeFiles }) {
  const code = [...mainSources.values()].join('\n');
  const members = jsMemberNames(code);
  const strings = new Set();
  for (const buf of nativeBinaries.values()) {
    for (const s of binaryIdentifiers(buf)) strings.add(s);
  }
  const candidates = [...strings].filter((s) => members.has(s)).sort();
  return {
    nodeFiles: [...new Set(nodeFiles.map((f) => f.replace(/\\/g, '/')).filter((f) => !IGNORED_NODE_FILE.test(f)))].sort(),
    candidates,
    used: usedNativeCalls(code),
  };
}

function exportedNames(nativeApi) {
  const names = new Set();
  for (const cls of Object.values(nativeApi.classes ?? {})) {
    for (const n of cls.statics ?? []) names.add(n);
    for (const n of cls.prototype ?? []) names.add(n);
  }
  return names;
}

/**
 * Compares a release surface with the baseline and the Linux addon exports.
 * Returns { ok, problems: string[] }.
 */
function checkSurface({ surface, baseline, nativeApi }) {
  const problems = [];
  const recorder = nativeApi.classes?.Recorder;
  const monitor = nativeApi.classes?.AudioMonitor;
  if (!recorder || !monitor) problems.push('native API description lacks Recorder or AudioMonitor');
  const has = (list, name) => (list ?? []).includes(name);
  for (const name of surface.used.recorderStatics) {
    if (name !== 'prototype' && !has(recorder?.statics, name)) problems.push(`Recorder.${name}() is not implemented by recorder_linux.node`);
  }
  for (const name of surface.used.recorderMethods) {
    if (!has(recorder?.prototype, name)) problems.push(`recorder.${name}() is not implemented by recorder_linux.node`);
  }
  for (const name of surface.used.monitorMethods) {
    if (!has(monitor?.prototype, name)) problems.push(`AudioMonitor#${name}() is not implemented by audio_monitor_linux.node`);
  }
  const exported = exportedNames(nativeApi);
  const known = new Set(baseline.candidates);
  for (const name of surface.candidates) {
    if (!known.has(name) && !exported.has(name)) {
      problems.push(`new native identifier "${name}" appears in the official addons and the main bundle`);
    }
  }
  const knownFiles = new Set(baseline.nodeFiles.map((f) => path.posix.basename(f)));
  for (const file of surface.nodeFiles) {
    if (!knownFiles.has(path.posix.basename(file))) problems.push(`release ships an unknown native addon: ${file}`);
  }
  return { ok: problems.length === 0, problems };
}

module.exports = { binaryIdentifiers, jsMemberNames, usedNativeCalls, computeSurface, checkSurface, exportedNames };
