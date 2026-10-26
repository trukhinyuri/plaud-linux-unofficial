'use strict';

// Minimal SemVer 2.0.0 parsing, comparison and range matching (">=1.4.1",
// ">=1.4.1 <2.0.0", "1.4.x"), enough for patch applicability and update
// version checks. No dependencies so it can run inside the app.

const SEMVER_RE = /^v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-.]+)?$/;

function parse(version) {
  const m = SEMVER_RE.exec(String(version).trim());
  if (!m) return null;
  return {
    major: Number(m[1]),
    minor: Number(m[2]),
    patch: Number(m[3]),
    prerelease: m[4] ? m[4].split('.') : [],
    version: `${m[1]}.${m[2]}.${m[3]}${m[4] ? `-${m[4]}` : ''}`,
  };
}

function mustParse(version) {
  const v = parse(version);
  if (!v) throw new TypeError(`invalid semantic version: ${version}`);
  return v;
}

function compareIdentifiers(a, b) {
  const an = /^\d+$/.test(a);
  const bn = /^\d+$/.test(b);
  if (an && bn) return Math.sign(Number(a) - Number(b));
  if (an) return -1;
  if (bn) return 1;
  return a < b ? -1 : a > b ? 1 : 0;
}

/** Returns -1, 0 or 1. */
function compare(a, b) {
  const x = mustParse(a);
  const y = mustParse(b);
  for (const k of ['major', 'minor', 'patch']) {
    if (x[k] !== y[k]) return Math.sign(x[k] - y[k]);
  }
  if (x.prerelease.length === 0 && y.prerelease.length === 0) return 0;
  if (x.prerelease.length === 0) return 1;
  if (y.prerelease.length === 0) return -1;
  for (let i = 0; i < Math.max(x.prerelease.length, y.prerelease.length); i++) {
    if (x.prerelease[i] === undefined) return -1;
    if (y.prerelease[i] === undefined) return 1;
    const c = compareIdentifiers(x.prerelease[i], y.prerelease[i]);
    if (c !== 0) return c;
  }
  return 0;
}

const gt = (a, b) => compare(a, b) > 0;
const gte = (a, b) => compare(a, b) >= 0;
const lt = (a, b) => compare(a, b) < 0;

function isPrerelease(version) {
  return mustParse(version).prerelease.length > 0;
}

/**
 * Space-separated comparators, all of which must hold: ">=1.4.1 <2.0.0".
 * "*" matches everything. "1.4.x" is accepted as ">=1.4.0 <1.5.0".
 */
function satisfies(version, range) {
  const v = mustParse(version);
  const plain = `${v.major}.${v.minor}.${v.patch}`;
  for (const raw of String(range).trim().split(/\s+/)) {
    if (raw === '*' || raw === '') continue;
    const x = /^(\d+)\.(\d+)\.x$/.exec(raw);
    if (x) {
      if (v.major !== Number(x[1]) || v.minor !== Number(x[2])) return false;
      continue;
    }
    const m = /^(>=|<=|>|<|=)?(.+)$/.exec(raw);
    const op = m[1] ?? '=';
    const c = compare(plain, m[2]);
    const ok = { '>=': c >= 0, '<=': c <= 0, '>': c > 0, '<': c < 0, '=': c === 0 }[op];
    if (!ok) return false;
  }
  return true;
}

module.exports = { parse, compare, gt, gte, lt, isPrerelease, satisfies };
