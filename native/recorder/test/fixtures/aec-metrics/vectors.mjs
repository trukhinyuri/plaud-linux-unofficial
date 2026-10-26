// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

// Deterministic synthetic inputs for the Recorder.computeAecLightMetrics
// conformance test. The same module drives the oracle capture against the
// official macOS addon (capture-oracle.mjs) and the Linux test, so both sides
// see bit-identical audio. Only integer ops and IEEE-754 + - * / are used
// (no Math.sin/exp/log), and every generated buffer is pinned by a sha256 in
// expected.json, so a generator drift fails loudly instead of comparing
// different inputs.

import { createHash } from 'node:crypto';

export function mulberry32(seed) {
  let a = seed >>> 0;
  return function next() {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Approximately Gaussian, zero mean, unit variance (Irwin-Hall with 12 terms).
function gauss(rng) {
  let s = 0;
  for (let i = 0; i < 12; i++) s += rng();
  return s - 6;
}

// sin/cos by Taylor series: deterministic on every IEEE-754 platform.
function taylorSin(x) {
  let term = x;
  let sum = x;
  for (let k = 1; k < 30; k++) {
    term = (-term * x * x) / ((2 * k) * (2 * k + 1));
    sum += term;
  }
  return sum;
}
function taylorCos(x) {
  let term = 1;
  let sum = 1;
  for (let k = 1; k < 30; k++) {
    term = (-term * x * x) / ((2 * k - 1) * (2 * k));
    sum += term;
  }
  return sum;
}

const TWO_PI = 6.283185307179586;

// Sinusoid of `freq` Hz at `rate` Hz via the Chebyshev recurrence.
function tone(n, freq, rate) {
  const w = (TWO_PI * freq) / rate;
  const c2 = 2 * taylorCos(w);
  const out = new Float64Array(n);
  if (n > 1) out[1] = taylorSin(w);
  for (let i = 2; i < n; i++) out[i] = c2 * out[i - 1] - out[i - 2];
  return out;
}

// 0..1..0 triangle at `hz`, used as a syllable-like amplitude envelope.
function tri(t, hz) {
  const p = t * hz - Math.floor(t * hz);
  return p < 0.5 ? 2 * p : 2 - 2 * p;
}

// 1 inside any [a, b) segment with 20 ms linear ramps, else 0.
function gate(t, segments) {
  const ramp = 0.02;
  let g = 0;
  for (const [a, b] of segments) {
    if (t < a || t >= b) continue;
    const up = (t - a) / ramp;
    const down = (b - t) / ramp;
    let v = up < down ? up : down;
    if (v > 1) v = 1;
    if (v > g) g = v;
  }
  return g;
}

/**
 * Synthetic AEC scene at `baseRate`: far-end (loopback) noise bursts, mic =
 * delayed/attenuated echo + near-end harmonic "speech" + noise floor, enh =
 * mic with the echo mostly removed, a ramping AGC gain on the near end and
 * reduced noise. Streams are decimated by integer factors to model different
 * device rates.
 */
function scene(p) {
  const R = p.baseRate;
  const N = Math.round(p.dur * R);
  const rng = mulberry32(p.seed);
  const far = p.far ?? [[0.5, 1.3], [2.3, 2.8]];
  const near = p.near ?? [[1.5, 2.3], [2.4, 2.75]];
  const D = p.delay; // echo delay in base-rate samples
  const pad = 64;
  const x = new Float64Array(N + 2 * D + pad + 16);
  for (let i = 0; i < x.length; i++) {
    const t = (i - D - pad) / R;
    const env = gate(t, far) * (0.55 + 0.45 * tri(t, 3));
    x[i] = env === 0 ? 0 : p.farAmp * env * gauss(rng);
  }
  const f0 = p.f0 ?? 180;
  const t1 = tone(N, f0, R);
  const t2 = tone(N, 2 * f0, R);
  const t3 = tone(N, 3 * f0, R);
  const lpb = new Float64Array(N);
  const mic = new Float64Array(N);
  const enh = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    const t = i / R;
    // Far-end as played (loopback). Normally the mic hears it D samples later;
    // with micLeads the mic is D samples ahead (negative delay).
    const lp = x[i + pad + D];
    const echo = p.micLeads
      ? p.echoGain * x[i + pad + 2 * D] + p.echoGain2 * x[i + pad + 2 * D - 7]
      : p.echoGain * x[i + pad] + p.echoGain2 * x[i + pad - 7];
    const ng = gate(t, near) * (0.35 + 0.65 * tri(t, 4.3));
    const speech = ng === 0 ? 0 : p.nearAmp * ng * (t1[i] + 0.5 * t2[i] + 0.25 * t3[i]);
    const noise = p.noiseAmp * gauss(rng);
    let agc = 1 + (p.agcSlope ?? 0.8) * (t - 1.5);
    if (agc < 1) agc = 1;
    if (agc > 1.7) agc = 1.7;
    lpb[i] = lp;
    mic[i] = echo + speech + noise;
    let e = p.residual * echo + agc * speech + p.enhNoise * noise;
    if (p.clipGain) {
      e *= p.clipGain;
      if (e > 1.25) e = 1.25;
      if (e < -1.25) e = -1.25;
    }
    enh[i] = e;
  }
  const dec = (src, fac, trim) => {
    const len = Math.floor(src.length / fac) - (trim ?? 0);
    const out = new Float32Array(len > 0 ? len : 0);
    for (let i = 0; i < out.length; i++) out[i] = src[i * fac];
    return out;
  };
  return {
    lpb: dec(lpb, p.lpbFac ?? 1, p.lpbTrim),
    mic: dec(mic, p.micFac ?? 1, p.micTrim),
    enh: dec(enh, p.enhFac ?? 1, p.enhTrim),
    lpbSr: R / (p.lpbFac ?? 1),
    micSr: R / (p.micFac ?? 1),
    enhSr: R / (p.enhFac ?? 1),
  };
}

function constant(n, v) {
  const a = new Float32Array(n);
  a.fill(v);
  return a;
}

function noiseOnly(n, seed, amp) {
  const rng = mulberry32(seed);
  const a = new Float32Array(n);
  for (let i = 0; i < n; i++) a[i] = amp * gauss(rng);
  return a;
}

const BASE16 = {
  baseRate: 16000, dur: 3, seed: 1, delay: 37, farAmp: 0.25, echoGain: 0.5, echoGain2: 0.15,
  nearAmp: 0.09, noiseAmp: 0.0015, residual: 0.03, enhNoise: 0.3,
};

const SCENES = {
  s16: () => scene(BASE16),
  s16Trim: () => scene({ ...BASE16, seed: 2, delay: 53, micTrim: 123, enhTrim: 45 }),
  s16Lead: () => scene({ ...BASE16, seed: 3, delay: 29, micLeads: true }),
  s16Clip: () => scene({ ...BASE16, seed: 4, clipGain: 9 }),
  s16Hot: () => scene({ ...BASE16, seed: 5, residual: 0.4, enhNoise: 0.9, agcSlope: 3 }),
  s48: () => scene({ ...BASE16, baseRate: 48000, dur: 2.9, seed: 6, delay: 113 }),
  s48mix: () => scene({ ...BASE16, baseRate: 48000, dur: 2.9, seed: 7, delay: 171, enhFac: 3, micTrim: 300 }),
  s48to16: () => scene({ ...BASE16, baseRate: 48000, dur: 2.9, seed: 8, delay: 150, lpbFac: 3, micFac: 3 }),
  s16NearOnly: () => scene({ ...BASE16, seed: 9, far: [] }),
  s16FarOnly: () => scene({ ...BASE16, seed: 10, near: [] }),
  zeros: () => ({ lpb: constant(8000, 0), mic: constant(8000, 0), enh: constant(8000, 0), lpbSr: 16000, micSr: 16000, enhSr: 16000 }),
  dc: () => ({ lpb: constant(6400, 0.5), mic: constant(6400, 0.25), enh: constant(6400, 0.125), lpbSr: 16000, micSr: 16000, enhSr: 16000 }),
  noise: () => ({ lpb: noiseOnly(9000, 11, 0.2), mic: noiseOnly(9000, 12, 0.01), enh: noiseOnly(9000, 13, 0.005), lpbSr: 16000, micSr: 16000, enhSr: 16000 }),
  short: () => ({ lpb: noiseOnly(150, 14, 0.3), mic: noiseOnly(150, 15, 0.3), enh: noiseOnly(150, 16, 0.3), lpbSr: 16000, micSr: 16000, enhSr: 16000 }),
  oneFrame: () => ({ lpb: noiseOnly(320, 17, 0.3), mic: noiseOnly(330, 18, 0.3), enh: noiseOnly(325, 19, 0.3), lpbSr: 16000, micSr: 16000, enhSr: 16000 }),
  tiny: () => ({ lpb: constant(1, 0.5), mic: constant(1, 0.5), enh: constant(1, 0.5), lpbSr: 16000, micSr: 16000, enhSr: 16000 }),
  emptyEnh: () => ({ lpb: noiseOnly(400, 20, 0.3), mic: noiseOnly(400, 21, 0.3), enh: new Float32Array(0), lpbSr: 16000, micSr: 16000, enhSr: 16000 }),
  resampleToZero: () => ({ lpb: noiseOnly(1, 22, 0.3), mic: noiseOnly(1, 23, 0.3), enh: noiseOnly(1, 24, 0.3), lpbSr: 48000, micSr: 48000, enhSr: 48000 }),
};

// Options sets. `undefined` means the 7th argument is omitted.
const OPTS = {
  none: undefined,
  empty: {},
  tuned: {
    frameMs: 32, hopMs: 16, maxDelayMs: 250, corrThresh: 0.4, lpbRelThreshDb: -30, delayEstimationMaxSec: 1.5,
    residualEchoCorrThresh: 0.2, noiseMaxDbAboveFloor: 4, speechMinDbAboveNoise: 8, ansSuppressionDbThreshold1: 2,
    ansSuppressionDbThreshold2: 10, agcSpeechTargetDbfs: -20, agcSpeechToleranceDb: 4, downsampleFactorForDelay: 2,
    targetSampleRate: 16000, minNoiseFrames: 5, minSpeechFrames: 5,
  },
  smallMins: { minNoiseFrames: 1, minSpeechFrames: 1 },
  zeroMins: { minNoiseFrames: 0, minSpeechFrames: 0 },
  hugeMins: { minNoiseFrames: 400, minSpeechFrames: 400 },
  wrongTypes: { frameMs: '20', hopMs: null, maxDelayMs: true, corrThresh: [0.1], targetSampleRate: '8000', minNoiseFrames: 3.9, minSpeechFrames: -7 },
  frame0: { frameMs: 0 },
  hopNeg: { hopMs: -5 },
  frameNaN: { frameMs: NaN },
  frameHuge: { frameMs: 100000 },
  frameOdd: { frameMs: 21.03, hopMs: 7.77 },
  resClampHi: { residualEchoCorrThresh: 2 },
  resClampLo: { residualEchoCorrThresh: -1, corrThresh: -1 },
  resNaN: { residualEchoCorrThresh: NaN, corrThresh: NaN },
  lpbInf: { lpbRelThreshDb: Infinity },
  lpbNegInf: { lpbRelThreshDb: -Infinity },
  lpbZero: { lpbRelThreshDb: 0 },
  agcTolNeg: { agcSpeechToleranceDb: -3, agcSpeechTargetDbfs: -30 },
  ansNeg: { ansSuppressionDbThreshold1: -5, ansSuppressionDbThreshold2: NaN, noiseMaxDbAboveFloor: -2, speechMinDbAboveNoise: -1 },
  delayUnbounded: { delayEstimationMaxSec: 0 },
  delayShortWin: { delayEstimationMaxSec: 0.05 },
  delayNaNWin: { delayEstimationMaxSec: NaN },
  ds0: { downsampleFactorForDelay: 0 },
  ds1: { downsampleFactorForDelay: 1 },
  ds3: { downsampleFactorForDelay: 3 },
  ds8: { downsampleFactorForDelay: 8 },
  dsHuge: { downsampleFactorForDelay: 1000 },
  maxDelay0: { maxDelayMs: 0 },
  maxDelayNeg: { maxDelayMs: -10 },
  maxDelay2ms: { maxDelayMs: 2 },
  maxDelayHuge: { maxDelayMs: 1e9 },
  tsr8k: { targetSampleRate: 8000 },
  tsr44k: { targetSampleRate: 44100 },
  tsr48k: { targetSampleRate: 48000 },
  tsrNeg: { targetSampleRate: -16000 },
  inherited: Object.create({ frameMs: 40, hopMs: 20 }),
  array: [],
};

// [caseId, scene, options, argument form]
// Argument forms: f32 (Float32Array), buffer (Node Buffer view of the float
// bytes), sub (unaligned Buffer copy at byte offset 1).
export const CASES = [
  ['s16-none', 's16', 'none', 'f32'],
  ['s16-empty', 's16', 'empty', 'f32'],
  ['s16-tuned', 's16', 'tuned', 'f32'],
  ['s16-buffer', 's16', 'none', 'buffer'],
  ['s16-unaligned', 's16', 'none', 'sub'],
  ['s16-smallMins', 's16', 'smallMins', 'f32'],
  ['s16-zeroMins', 's16', 'zeroMins', 'f32'],
  ['s16-hugeMins', 's16', 'hugeMins', 'f32'],
  ['s16-wrongTypes', 's16', 'wrongTypes', 'f32'],
  ['s16-frame0', 's16', 'frame0', 'f32'],
  ['s16-hopNeg', 's16', 'hopNeg', 'f32'],
  ['s16-frameNaN', 's16', 'frameNaN', 'f32'],
  ['s16-frameHuge', 's16', 'frameHuge', 'f32'],
  ['s16-frameOdd', 's16', 'frameOdd', 'f32'],
  ['s16-resClampHi', 's16', 'resClampHi', 'f32'],
  ['s16-resClampLo', 's16', 'resClampLo', 'f32'],
  ['s16-resNaN', 's16', 'resNaN', 'f32'],
  ['s16-lpbInf', 's16', 'lpbInf', 'f32'],
  ['s16-lpbNegInf', 's16', 'lpbNegInf', 'f32'],
  ['s16-lpbZero', 's16', 'lpbZero', 'f32'],
  ['s16-agcTolNeg', 's16', 'agcTolNeg', 'f32'],
  ['s16-ansNeg', 's16', 'ansNeg', 'f32'],
  ['s16-delayUnbounded', 's16', 'delayUnbounded', 'f32'],
  ['s16-delayShortWin', 's16', 'delayShortWin', 'f32'],
  ['s16-delayNaNWin', 's16', 'delayNaNWin', 'f32'],
  ['s16-ds0', 's16', 'ds0', 'f32'],
  ['s16-ds1', 's16', 'ds1', 'f32'],
  ['s16-ds3', 's16', 'ds3', 'f32'],
  ['s16-ds8', 's16', 'ds8', 'f32'],
  ['s16-dsHuge', 's16', 'dsHuge', 'f32'],
  ['s16-maxDelay0', 's16', 'maxDelay0', 'f32'],
  ['s16-maxDelayNeg', 's16', 'maxDelayNeg', 'f32'],
  ['s16-maxDelay2ms', 's16', 'maxDelay2ms', 'f32'],
  ['s16-maxDelayHuge', 's16', 'maxDelayHuge', 'f32'],
  ['s16-tsr8k', 's16', 'tsr8k', 'f32'],
  ['s16-tsrNeg', 's16', 'tsrNeg', 'f32'],
  ['s16-inherited', 's16', 'inherited', 'f32'],
  ['s16-array', 's16', 'array', 'f32'],
  ['s16Trim-none', 's16Trim', 'none', 'f32'],
  ['s16Lead-none', 's16Lead', 'none', 'f32'],
  ['s16Lead-tuned', 's16Lead', 'tuned', 'f32'],
  ['s16Clip-none', 's16Clip', 'none', 'f32'],
  ['s16Hot-none', 's16Hot', 'none', 'f32'],
  ['s16Hot-smallMins', 's16Hot', 'smallMins', 'f32'],
  ['s48-none', 's48', 'none', 'f32'],
  ['s48-tsr44k', 's48', 'tsr44k', 'f32'],
  ['s48-tsr8k', 's48', 'tsr8k', 'f32'],
  ['s48mix-none', 's48mix', 'none', 'f32'],
  ['s48mix-tuned', 's48mix', 'tuned', 'f32'],
  ['s48mix-tsr48k', 's48mix', 'tsr48k', 'f32'],
  ['s48to16-none', 's48to16', 'none', 'f32'],
  ['s48to16-tsr48k', 's48to16', 'tsr48k', 'f32'],
  ['s16NearOnly-none', 's16NearOnly', 'none', 'f32'],
  ['s16NearOnly-smallMins', 's16NearOnly', 'smallMins', 'f32'],
  ['s16FarOnly-none', 's16FarOnly', 'none', 'f32'],
  ['zeros-none', 'zeros', 'none', 'f32'],
  ['zeros-zeroMins', 'zeros', 'zeroMins', 'f32'],
  ['dc-none', 'dc', 'none', 'f32'],
  ['dc-smallMins', 'dc', 'smallMins', 'f32'],
  ['noise-none', 'noise', 'none', 'f32'],
  ['noise-smallMins', 'noise', 'smallMins', 'f32'],
  ['short-none', 'short', 'none', 'f32'],
  ['oneFrame-none', 'oneFrame', 'none', 'f32'],
  ['oneFrame-zeroMins', 'oneFrame', 'zeroMins', 'f32'],
  ['tiny-none', 'tiny', 'none', 'f32'],
  ['emptyEnh-none', 'emptyEnh', 'none', 'f32'],
  ['resampleToZero-tsr8k', 'resampleToZero', 'tsr8k', 'f32'],
];

const sceneCache = new Map();
export function getScene(name) {
  if (!sceneCache.has(name)) sceneCache.set(name, SCENES[name]());
  return sceneCache.get(name);
}

export function sceneHashes(name) {
  const s = getScene(name);
  const h = (a) => createHash('sha256').update(new Uint8Array(a.buffer, a.byteOffset, a.byteLength)).digest('hex');
  return { lpb: h(s.lpb), mic: h(s.mic), enh: h(s.enh), lpbSr: s.lpbSr, micSr: s.micSr, enhSr: s.enhSr };
}

function asForm(a, form) {
  if (form === 'f32') return a;
  if (form === 'buffer') return Buffer.from(a.buffer, a.byteOffset, a.byteLength);
  if (form === 'sub') {
    const raw = Buffer.alloc(a.byteLength + 1);
    Buffer.from(a.buffer, a.byteOffset, a.byteLength).copy(raw, 1);
    return raw.subarray(1);
  }
  throw new Error(`unknown form ${form}`);
}

export function caseArgs([, sceneName, optName, form]) {
  const s = getScene(sceneName);
  const args = [asForm(s.lpb, form), s.lpbSr, asForm(s.mic, form), s.micSr, asForm(s.enh, form), s.enhSr];
  const opts = OPTS[optName];
  if (opts !== undefined || optName !== 'none') args.push(opts);
  return args;
}

// Argument-validation and type-acceptance probes. Each builds a fresh
// argument list; the expectation is either an exact error (class + message)
// or a result object.
const f = (n, seed) => noiseOnly(n, seed, 0.2);
const okBufs = () => [f(2000, 31), 16000, f(2000, 32), 16000, f(2000, 33), 16000];
function withArg(i, v) {
  const a = okBufs();
  a[i] = v;
  return a;
}

// Options object that records every property probe the addon makes (has/get
// trap order and repetition) while serving numeric values for all options.
const accessLog = [];
function traceProxy() {
  const values = {
    frameMs: 20, hopMs: 10, maxDelayMs: 500, corrThresh: 0.6, lpbRelThreshDb: -40, delayEstimationMaxSec: 10,
    residualEchoCorrThresh: 0.3, noiseMaxDbAboveFloor: 3, speechMinDbAboveNoise: 10, ansSuppressionDbThreshold1: 3,
    ansSuppressionDbThreshold2: 6, agcSpeechTargetDbfs: -18, agcSpeechToleranceDb: 6, downsampleFactorForDelay: 4,
    targetSampleRate: 16000, minNoiseFrames: 20, minSpeechFrames: 20,
  };
  return new Proxy(values, {
    has(t, k) { accessLog.push(`has:${String(k)}`); return Reflect.has(t, k); },
    get(t, k, r) { accessLog.push(`get:${String(k)}`); return Reflect.get(t, k, r); },
  });
}
export const PROBES = {
  noArgs: () => [],
  fiveArgs: () => okBufs().slice(0, 5),
  sixArgs: () => okBufs(),
  sevenUndefined: () => [...okBufs(), undefined],
  sevenNull: () => [...okBufs(), null],
  sevenNumber: () => [...okBufs(), 5],
  sevenString: () => [...okBufs(), 'frameMs'],
  sevenFunction: () => [...okBufs(), Object.assign(function opts() {}, { frameMs: 40, hopMs: 20 })],
  eightArgs: () => [...okBufs(), {}, 'extra'],
  lpbNumber: () => withArg(0, 5),
  lpbString: () => withArg(0, 'abc'),
  lpbNull: () => withArg(0, null),
  lpbUndefined: () => withArg(0, undefined),
  lpbArray: () => withArg(0, [0.1, 0.2, 0.3, 0.4]),
  lpbArrayBuffer: () => withArg(0, new ArrayBuffer(64)),
  lpbObject: () => withArg(0, { length: 4 }),
  lpbDataView: () => withArg(0, new DataView(new ArrayBuffer(64))),
  lpbFloat64Zeros: () => withArg(0, new Float64Array(2000)),
  lpbInt16: () => withArg(0, new Int16Array(4000)),
  lpbUint8Odd: () => withArg(0, new Uint8Array(4003)),
  lpbBigInt64: () => withArg(0, new BigInt64Array(1000)),
  lpbSrString: () => withArg(1, '16000'),
  lpbSrBigInt: () => withArg(1, 16000n),
  lpbSrBool: () => withArg(1, true),
  lpbSrNumberObject: () => withArg(1, Object(16000)),
  lpbSrUndefined: () => withArg(1, undefined),
  lpbSrNaN: () => withArg(1, NaN),
  lpbSrNeg: () => withArg(1, -16000),
  lpbSrFrac: () => withArg(1, 16000.9),
  lpbSrWrap: () => withArg(1, 4294967296 + 16000),
  micBufBad: () => withArg(2, 'x'),
  micSrBad: () => withArg(3, null),
  micSrZero: () => withArg(3, 0),
  enhBufBad: () => withArg(4, {}),
  enhSrBad: () => withArg(5, '16000'),
  enhSrNeg: () => withArg(5, -1),
  orderLpbBufFirst: () => { const a = okBufs(); a[0] = 1; a[1] = 'x'; a[2] = 2; return a; },
  orderSrBeforeMicBuf: () => { const a = okBufs(); a[1] = 'x'; a[2] = 2; return a; },
  orderMicBeforeEnh: () => { const a = okBufs(); a[2] = 1; a[4] = 2; a[5] = 'x'; return a; },
  lpbSrZeroTsr: () => [...withArg(1, 0), { targetSampleRate: 16000 }],
  lpbSrZeroNoTsr: () => withArg(1, 0),
  emptyAll: () => [new Float32Array(0), 16000, new Float32Array(0), 16000, new Float32Array(0), 16000],
  optThrowingGetter: () => [...okBufs(), { get frameMs() { throw new RangeError('getter boom'); } }],
  optThrowingIntGetter: () => [...okBufs(), { get minNoiseFrames() { throw new TypeError('int getter boom'); } }],
  optThrowingHasTrap: () => [...okBufs(), new Proxy({}, { has() { throw new Error('has boom'); } })],
  lpbSrInfinity: () => withArg(1, Infinity),
  lpbFloat16: () => (typeof Float16Array === 'function' ? withArg(0, new Float16Array(4000)) : withArg(0, new Uint16Array(4000))),
  optAccessTrace: () => [...okBufs(), traceProxy()],
};

// JSON cannot carry NaN, +-Infinity or -0; tag them as strings.
export function encodeNumbers(_k, v) {
  if (typeof v === 'number' && (!Number.isFinite(v) || Object.is(v, -0))) return `__num:${Object.is(v, -0) ? '-0' : v}`;
  return v;
}
export function decodeNumbers(_k, v) {
  if (typeof v === 'string' && v.startsWith('__num:')) {
    const s = v.slice(6);
    return s === '-0' ? -0 : Number(s);
  }
  return v;
}

export function runCall(fn, args) {
  accessLog.length = 0;
  let outcome;
  try {
    outcome = { kind: 'result', value: fn(...args) };
  } catch (e) {
    outcome = { kind: 'error', name: e?.constructor?.name ?? typeof e, message: String(e?.message ?? e) };
  }
  if (accessLog.length) outcome.optionAccess = [...accessLog];
  return outcome;
}
