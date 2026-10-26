'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const patcher = require('../patcher');
const { AsarArchive } = require('../patcher/lib/asar');
const { decodePng } = require('../patcher/lib/png');
const { checkSurface } = require('../patcher/lib/api-surface');
const constants = require('../linux-runtime/constants');
const { REPO_ROOT, pinned, officialResources, SKIP_REASON } = require('./helpers/official');

const resources = officialResources();
const skip = resources ? false : SKIP_REASON;

/** In-memory stand-in for AsarArchive (listFiles/readFile/writeFile). */
function memoryArchive(files) {
  const map = new Map(Object.entries(files).map(([k, v]) => [k, Buffer.from(v)]));
  return {
    listFiles: () => [...map.keys()],
    readFile: (f) => map.get(f),
    writeFile: (f, data) => map.set(f, Buffer.from(data)),
    text: (f) => map.get(f).toString('utf8'),
  };
}

const ONE_PATCH = [{ id: 'demo', file: 'main/a-*.js', versions: '>=1.0.0', anchor: 'OLD()', replacement: 'NEW()' }];

test('a patch applies exactly once and records hashes', () => {
  const archive = memoryArchive({ 'main/a-1.js': 'x(); OLD(); y();', 'main/b.js': 'OLD();' });
  const records = patcher.applyPatches(archive, '1.0.0', ONE_PATCH);
  assert.equal(archive.text('main/a-1.js'), 'x(); NEW(); y();');
  assert.equal(archive.text('main/b.js'), 'OLD();', 'files outside the glob stay untouched');
  assert.equal(records.length, 1);
  assert.notEqual(records[0].sha256Before, records[0].sha256After);
});

test('a missing anchor is refused, and recognised when the input is already patched', () => {
  assert.throws(() => patcher.applyPatches(memoryArchive({ 'main/a-1.js': 'nothing here' }), '1.0.0', ONE_PATCH), {
    name: 'PatchError',
    code: 'ANCHOR_NOT_FOUND',
  });
  assert.throws(() => patcher.applyPatches(memoryArchive({ 'main/a-1.js': 'NEW();' }), '1.0.0', ONE_PATCH), /already patched/);
});

test('an ambiguous anchor is refused, also across files', () => {
  assert.throws(() => patcher.applyPatches(memoryArchive({ 'main/a-1.js': 'OLD(); OLD();' }), '1.0.0', ONE_PATCH), {
    code: 'ANCHOR_AMBIGUOUS',
  });
  assert.throws(() => patcher.applyPatches(memoryArchive({ 'main/a-1.js': 'OLD();', 'main/a-2.js': 'OLD();' }), '1.0.0', ONE_PATCH), {
    code: 'ANCHOR_AMBIGUOUS',
  });
});

test('a release outside every patch version range is unsupported', () => {
  assert.throws(() => patcher.selectPatches('1.3.0'), { code: 'UNSUPPORTED_VERSION' });
});

test('patch declarations are complete', () => {
  const ids = new Set();
  for (const patch of patcher.PATCHES) {
    for (const key of ['id', 'file', 'versions', 'anchor', 'replacement', 'rationale', 'evidence']) {
      assert.ok(typeof patch[key] === 'string' && patch[key].length > 0, `${patch.id ?? '?'}: ${key}`);
    }
    // Insertion patches keep the anchor and add code next to it.
    assert.notEqual(patch.replacement, patch.anchor, `${patch.id}: the replacement changes something`);
    ids.add(patch.id);
  }
  assert.equal(patcher.selectPatches(pinned.version).length, ids.size);
});

/** Parses "file.js:12, 30-45" into { file, ranges: [[12, 12], [30, 45]] }. */
function parseEvidence(evidence) {
  const [file, spec] = evidence.split(':');
  const ranges = spec.split(',').map((part) => {
    const [a, b] = part.trim().split('-').map(Number);
    return [a, b ?? a];
  });
  return { file, ranges };
}

test(`every anchor occurs exactly once in Plaud ${pinned.version}, within the documented lines`, { skip }, () => {
  const archive = AsarArchive.open(path.join(resources, 'app.asar'));
  try {
    const files = archive.listFiles();
    for (const patch of patcher.selectPatches(pinned.version)) {
      const re = patcher.globToRegExp(patch.file);
      const hits = files
        .filter((f) => re.test(f))
        .map((f) => ({ f, n: patcher.countOccurrences(archive.readFile(f).toString('utf8'), patch.anchor) }))
        .filter((h) => h.n > 0);
      assert.equal(hits.reduce((n, h) => n + h.n, 0), 1, `${patch.id}: ${JSON.stringify(hits)}`);
      const evidence = parseEvidence(patch.evidence);
      assert.equal(path.posix.basename(hits[0].f), evidence.file, `${patch.id}: evidence names the patched file`);
      const source = archive.readFile(hits[0].f).toString('utf8');
      const line = source.slice(0, source.indexOf(patch.anchor)).split('\n').length;
      assert.ok(
        evidence.ranges.some(([a, b]) => line >= a && line <= b),
        `${patch.id}: anchor is on line ${line}, evidence cites ${patch.evidence}`,
      );
    }
  } finally {
    archive.close();
  }
});

test(`patchResources produces a self-contained Linux archive from Plaud ${pinned.version}`, { skip }, () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'plaud-patch-'));
  const out = path.join(tmp, 'app.asar');
  try {
    const result = patcher.patchResources({
      resourcesDir: resources,
      outAsar: out,
      runtimeDir: path.join(REPO_ROOT, 'linux-runtime'),
      runtimeId: 'test-runtime',
      computeBaseline: true,
    });
    assert.equal(result.version, pinned.version);
    assert.equal(result.manifest.patches.length, patcher.selectPatches(pinned.version).length);
    assert.ok(!fs.existsSync(`${out}.unpacked`), 'nothing is left unpacked');

    const archive = AsarArchive.open(out);
    try {
      const pkg = JSON.parse(archive.readFile('package.json').toString('utf8'));
      assert.equal(pkg.main, patcher.LOADER);
      assert.equal(pkg.desktopName, `${constants.APP_ID}.desktop`);
      assert.equal(pkg.version, pinned.version);
      assert.deepEqual(pkg.plaudLinux, { upstreamMain: result.manifest.upstreamMain, upstreamVersion: pinned.version, runtimeId: 'test-runtime' });
      assert.ok(archive.has(pkg.plaudLinux.upstreamMain.replace(/^\.\//, '')), 'the official entry point is still present');

      const manifest = JSON.parse(archive.readFile(patcher.MANIFEST).toString('utf8'));
      assert.equal(manifest.runtimeId, 'test-runtime');

      for (const name of ['off', 'on', 'ppc', 'update']) {
        const png = decodePng(archive.readFile(`resources/tray/linux/Icon-${name}.png`));
        assert.equal(png.width, 64);
        assert.equal(png.height, 64);
      }

      const files = archive.listFiles();
      assert.deepEqual(files.filter((f) => /\.(node|dylib)$/.test(f)), [], 'no macOS binaries remain');
      assert.deepEqual(files.filter((f) => archive.get(f).unpacked), [], 'no unpacked entries');
      for (const runtimeFile of fs.readdirSync(path.join(REPO_ROOT, 'linux-runtime')).filter((f) => f.endsWith('.js'))) {
        assert.ok(archive.has(`linux-runtime/${runtimeFile}`), `linux-runtime/${runtimeFile} injected`);
      }

      for (const record of manifest.patches) {
        const patch = patcher.selectPatches(pinned.version).find((p) => p.id === record.id);
        const source = archive.readFile(record.file).toString('utf8');
        assert.equal(patcher.countOccurrences(source, patch.replacement), 1, `${record.id} applied once`);
        const expectedAnchors = patch.replacement.includes(patch.anchor) ? 1 : 0;
        assert.equal(patcher.countOccurrences(source, patch.anchor), expectedAnchors, `${record.id} anchor handled`);
      }
      // Every patched and injected file still compiles.
      const toCompile = new Set([...manifest.patches.map((p) => p.file), ...files.filter((f) => f.startsWith('linux-runtime/') && f.endsWith('.js'))]);
      for (const file of toCompile) {
        assert.doesNotThrow(() => new vm.Script(`(function (exports, require, module, __filename, __dirname) {${archive.readFile(file)}\n})`, { filename: file }), file);
      }
    } finally {
      archive.close();
    }

    // The patched archive is recognised and not patched twice.
    const again = path.join(tmp, 'again');
    fs.mkdirSync(again);
    fs.copyFileSync(out, path.join(again, 'app.asar'));
    assert.throws(
      () => patcher.patchResources({ resourcesDir: again, outAsar: path.join(tmp, 'x.asar'), runtimeDir: path.join(REPO_ROOT, 'linux-runtime'), runtimeId: 'x', computeBaseline: true }),
      { code: 'ALREADY_PATCHED' },
    );

    // The release's own surface is accepted by an addon API that covers it,
    // and a missing method is reported.
    const baseline = { nodeFiles: result.surface.nodeFiles, candidates: result.surface.candidates, used: result.surface.used };
    const api = {
      classes: {
        Recorder: { statics: [...result.surface.used.recorderStatics], prototype: [...result.surface.used.recorderMethods] },
        AudioMonitor: { statics: [], prototype: [...result.surface.used.monitorMethods] },
      },
    };
    assert.deepEqual(checkSurface({ surface: result.surface, baseline, nativeApi: api }), { ok: true, problems: [] });
    api.classes.Recorder.prototype = api.classes.Recorder.prototype.filter((m) => m !== 'pause');
    const missing = checkSurface({ surface: result.surface, baseline, nativeApi: api });
    assert.equal(missing.ok, false);
    assert.match(missing.problems.join('\n'), /recorder\.pause\(\)/);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});
