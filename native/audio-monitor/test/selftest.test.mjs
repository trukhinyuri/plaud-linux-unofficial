// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
//
// Runs the native pure-logic suite (selectors, meeting state machine, app
// catalog, /proc/net/udp parsing, window titles, JSON, level meter).
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { test } from 'node:test';
import { selftestPath } from './helpers.mjs';

test('native selftest', (t) => {
  const r = spawnSync(selftestPath, [], { encoding: 'utf8', timeout: 60000 });
  assert.equal(r.error, undefined, String(r.error));
  const lines = r.stdout.split('\n').filter(Boolean);
  const groups = lines.filter((l) => l.startsWith('ok ') || l.startsWith('not ok '));
  assert.ok(groups.length >= 15, `expected the selftest groups, got:\n${r.stdout}\n${r.stderr}`);
  for (const line of groups) t.diagnostic(line);
  const failures = lines.filter((l) => l.startsWith('not ok ') || l.startsWith('#   FAIL'));
  assert.deepEqual(failures, [], r.stdout);
  assert.equal(r.status, 0, r.stdout + r.stderr);
});
