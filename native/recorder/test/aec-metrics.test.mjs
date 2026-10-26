// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

// Conformance of Recorder.computeAecLightMetrics with the official Plaud
// Desktop addon. Inputs are regenerated deterministically by
// fixtures/aec-metrics/vectors.mjs (pinned by sha256); expected outputs and
// errors were captured from recorder_mac.node by capture-oracle.mjs.

import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  CASES,
  PROBES,
  caseArgs,
  decodeNumbers,
  runCall,
  sceneHashes,
} from './fixtures/aec-metrics/vectors.mjs';

const require = createRequire(import.meta.url);
const addonPath = process.env.RECORDER_ADDON
  ? path.resolve(process.env.RECORDER_ADDON)
  : fileURLToPath(new URL('../build/Release/recorder_linux.node', import.meta.url));
const { Recorder } = require(addonPath);

const expected = JSON.parse(
  readFileSync(new URL('./fixtures/aec-metrics/expected.json', import.meta.url), 'utf8'),
  decodeNumbers,
);

// Integer fields must match exactly, as must null (absent) vs number and the
// key order. The implementation repeats the reference arithmetic operation by
// operation (same order, double precision, fused multiply-add at the same
// places), so fields computed only from + - * / sqrt and fma (all correctly
// rounded under IEEE 754) are bit-exact on every platform and asserted with
// Object.is: this catches rounding-order mistakes such as a missing or extra
// fused multiply-add. Fields that pass through log10, exp or hypot inherit
// platform libm differences: glibc and Apple libm disagree by 1 ulp on ~3% of
// log10, ~0.2% of exp and ~11% of hypot results (sin, cos, sqrt agree);
// propagated through means, standard deviations and dB ratios this reached
// 2.7e-15 relative / 1e-14 absolute over the whole fixture. Their tolerance
// keeps ~1000x headroom over that, while formula, threshold or ordering
// mistakes move results by orders of magnitude more.
const REL_TOL = 1e-12;
const ABS_TOL = 1e-12;
const INTEGER_KEYS = new Set(expected.keys.slice(0, 9));
const LIBM_FREE_KEYS = new Set([
  'farendActiveRatio',
  'echoDominantRatio',
  'nearendOnlyRatio',
  'meanAbsCorrMicFarendActive',
  'meanAbsCorrEnhFarendActive',
  'residualEchoRatioFarendActive',
  'meanAbsCorrMicEchoDominant',
  'meanAbsCorrEnhEchoDominant',
  'residualEchoRatioEchoDominant',
  'ansNoiseRatio',
  'agcSpeechRatio',
  'ansSuppressedNoiseRatio3db',
  'ansSuppressedNoiseRatio6db',
  'ansSuppressedTotalRatio3db',
  'ansSuppressedTotalRatio6db',
  'agcSpeechLevelInRangeRatio',
  'agcSpeechLevelInRangeTotalRatio',
  'enhClipFraction',
]);

function assertOutcome(actual, want, label) {
  assert.equal(actual.kind, want.kind, `${label}: outcome kind`);
  assert.deepEqual(actual.optionAccess, want.optionAccess, `${label}: options property access sequence`);
  if (want.kind === 'error') {
    assert.equal(actual.name, want.name, `${label}: error class`);
    assert.equal(actual.message, want.message, `${label}: error message`);
    return;
  }
  const keys = Object.keys(actual.value);
  assert.deepEqual(keys, expected.keys, `${label}: result keys and order`);
  keys.forEach((key, i) => {
    const got = actual.value[key];
    const exp = want.value[i];
    if (exp === null || got === null || INTEGER_KEYS.has(key) || LIBM_FREE_KEYS.has(key) || !Number.isFinite(exp)) {
      assert.ok(Object.is(got, exp), `${label}.${key}: expected ${exp}, got ${got}`);
      return;
    }
    assert.equal(typeof got, 'number', `${label}.${key}: type`);
    const diff = Math.abs(got - exp);
    assert.ok(
      diff <= ABS_TOL + REL_TOL * Math.abs(exp),
      `${label}.${key}: expected ${exp}, got ${got} (|diff| ${diff})`,
    );
  });
}

test('computeAecLightMetrics is a static function with the reference arity', () => {
  assert.equal(typeof Recorder.computeAecLightMetrics, 'function');
  assert.equal(Recorder.computeAecLightMetrics.length, expected.capturedWith.functionLength);
});

test('generated inputs are the ones the oracle saw', () => {
  for (const [name, hashes] of Object.entries(expected.scenes)) {
    assert.deepEqual(sceneHashes(name), hashes, `scene ${name}`);
  }
});

const call = (...args) => Recorder.computeAecLightMetrics(...args);

for (const c of CASES) {
  const [id] = c;
  test(`oracle case ${id}`, () => {
    const want = expected.cases[id];
    assert.ok(want, `fixture has case ${id}`);
    assertOutcome(runCall(call, caseArgs(c)), want.outcome, id);
  });
}

for (const [id, build] of Object.entries(PROBES)) {
  test(`argument probe ${id}`, () => {
    const want = expected.probes[id];
    assert.ok(want, `fixture has probe ${id}`);
    assertOutcome(runCall(call, build()), want, id);
  });
}
