#!/usr/bin/env node
'use strict';

// Patcher command line.
//
//   cli.js check  --resources <dir>
//       Applies the patches in memory to <dir>/app.asar and reports the result
//       (anchor test; no native API check).
//   cli.js patch  --resources <dir> --out <app.asar> --runtime-id <id>
//                 [--native-api <json>] [--baseline <json>] [--write-baseline <json>]
//       Writes a patched archive. Without --native-api/--baseline the API
//       surface of the input is computed (and optionally written) instead of
//       checked.
//   cli.js stage  --zip <official mac zip> --sha512 <base64> --version <v>
//                 --versions-dir <dir> --resources-dir <resources/plaud-linux>
//       Used by the in-app updater (run with ELECTRON_RUN_AS_NODE=1): verifies,
//       extracts, checks, patches and atomically stages a release in a private
//       directory (linux-runtime/secure-fs.js). stage.json, written last,
//       records the SHA-256 of every staged file for the loader. Prints one
//       JSON line { ok, ... } on stdout; exit status 0 (staged), 3 (refused)
//       or 1 (error).

const path = require('node:path');
const fs = require('./lib/fs');
const { openZip } = require('./lib/zip');
const { AsarArchive } = require('./lib/asar');
const patcher = require('./index');
const feed = require('../linux-runtime/feed');
const secure = require('../linux-runtime/secure-fs');

const MAC_RESOURCES = 'Plaud.app/Contents/Resources/';

function parseArgs(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) {
      const key = argv[i].slice(2);
      const next = argv[i + 1];
      if (next === undefined || next.startsWith('--')) out[key] = true;
      else {
        out[key] = next;
        i++;
      }
    } else {
      out._.push(argv[i]);
    }
  }
  return out;
}

function required(args, key) {
  if (typeof args[key] !== 'string' || args[key] === '') throw new Error(`missing --${key}`);
  return args[key];
}

const readJson = (file) => JSON.parse(fs.readFileSync(file, 'utf8'));

function writeJsonAtomic(file, value) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const tmp = `${file}.tmp-${process.pid}`;
  fs.writeFileSync(tmp, `${JSON.stringify(value, null, 2)}\n`);
  fs.renameSync(tmp, file);
}

/** Extracts the parts of the official mac zip the patcher needs. */
async function extractOfficialResources(zipPath, destDir) {
  const zip = openZip(zipPath);
  try {
    const wanted = (rel) =>
      rel === 'app.asar' || rel.startsWith('app.asar.unpacked/') || (!rel.includes('/') && (rel.endsWith('.node') || rel === 'icon.icns'));
    let count = 0;
    for (const entry of zip.entries) {
      if (!entry.name.startsWith(MAC_RESOURCES)) continue;
      const rel = entry.name.slice(MAC_RESOURCES.length);
      if (!rel || !wanted(rel)) continue;
      await zip.extract(entry, path.join(destDir, ...rel.split('/')));
      count++;
    }
    if (!fs.existsSync(path.join(destDir, 'app.asar'))) {
      throw new Error(`${zipPath} does not contain ${MAC_RESOURCES}app.asar`);
    }
    return count;
  } finally {
    zip.close();
  }
}

async function cmdCheck(args) {
  const resources = required(args, 'resources');
  const archive = AsarArchive.open(path.join(resources, 'app.asar'));
  try {
    const pkg = JSON.parse(archive.readFile('package.json').toString('utf8'));
    const records = patcher.applyPatches(archive, String(pkg.version));
    const width = Math.max(...records.map((r) => r.id.length));
    for (const r of records) process.stdout.write(`ok  ${r.id.padEnd(width)}  ${r.file}\n`);
    process.stdout.write(`all ${records.length} patches apply to Plaud ${pkg.version}\n`);
  } finally {
    archive.close();
  }
}

async function cmdPatch(args) {
  const resources = required(args, 'resources');
  const out = required(args, 'out');
  const runtimeId = required(args, 'runtime-id');
  const runtimeDir = typeof args['runtime-dir'] === 'string' ? args['runtime-dir'] : path.join(__dirname, '..', 'linux-runtime');
  const nativeApi = typeof args['native-api'] === 'string' ? readJson(args['native-api']) : undefined;
  const baseline = typeof args.baseline === 'string' ? readJson(args.baseline) : undefined;
  const result = patcher.patchResources({
    resourcesDir: resources,
    outAsar: out,
    runtimeDir,
    runtimeId,
    nativeApi,
    baseline,
    computeBaseline: !nativeApi || !baseline,
  });
  if (typeof args['write-baseline'] === 'string') {
    writeJsonAtomic(args['write-baseline'], {
      upstreamVersion: result.version,
      nodeFiles: result.surface.nodeFiles,
      candidates: result.surface.candidates,
      used: result.surface.used,
    });
  }
  process.stdout.write(`${JSON.stringify({ ok: true, version: result.version, patches: result.manifest.patches.length })}\n`);
}

async function cmdStage(args) {
  const zipPath = required(args, 'zip');
  const sha512 = required(args, 'sha512');
  const version = required(args, 'version');
  const versionsDir = required(args, 'versions-dir');
  const resourcesDir = required(args, 'resources-dir');
  const buildInfo = readJson(path.join(resourcesDir, 'build-info.json'));
  const nativeApi = readJson(path.join(resourcesDir, 'native-api.json'));
  const baseline = readJson(path.join(resourcesDir, 'api-baseline.json'));

  try {
    await feed.verifySha512(zipPath, sha512);
  } catch (error) {
    return { ok: false, code: 'SHA512_MISMATCH', message: error.message };
  }

  // versionsDir and its parent (the data directory of this build) are private.
  secure.ensurePrivateDir(versionsDir, path.dirname(path.dirname(versionsDir)));
  const work = fs.mkdtempSync(path.join(versionsDir, `.tmp-${version}-`));
  fs.chmodSync(work, 0o700);
  try {
    const extracted = path.join(work, 'official');
    await extractOfficialResources(zipPath, extracted);
    const outDir = path.join(work, 'out');
    secure.makePrivateDir(outDir);
    let result;
    try {
      result = patcher.patchResources({
        resourcesDir: extracted,
        outAsar: path.join(outDir, 'app.asar'),
        runtimeDir: path.join(resourcesDir, 'linux-runtime'),
        runtimeId: buildInfo.runtimeId,
        nativeApi,
        baseline,
      });
    } catch (error) {
      if (error instanceof patcher.PatchError) {
        return { ok: false, code: error.code ?? 'PATCH_FAILED', message: error.message, problems: error.problems };
      }
      throw error;
    }
    if (result.version !== version) {
      return { ok: false, code: 'VERSION_MISMATCH', message: `archive contains ${result.version}, feed announced ${version}` };
    }
    secure.writePrivateFile(
      path.join(outDir, 'stage.json'),
      `${JSON.stringify(
        {
          version,
          runtimeId: buildInfo.runtimeId,
          sourceSha512: sha512,
          stagedAt: new Date().toISOString(),
          patches: result.manifest.patches.map((p) => p.id),
          files: secure.hashTree(outDir),
        },
        null,
        2,
      )}\n`,
    );
    const finalDir = path.join(versionsDir, version);
    fs.rmSync(finalDir, { recursive: true, force: true });
    fs.renameSync(outDir, finalDir);
    return { ok: true, version, dir: finalDir };
  } finally {
    fs.rmSync(work, { recursive: true, force: true });
  }
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const command = args._[0];
  if (command === 'check') return cmdCheck(args);
  if (command === 'patch') return cmdPatch(args);
  if (command === 'stage') {
    let result;
    try {
      result = await cmdStage(args);
    } catch (error) {
      result = { ok: false, code: 'IO_ERROR', message: error.message };
    }
    process.stdout.write(`${JSON.stringify(result)}\n`);
    process.exitCode = result.ok ? 0 : result.code === 'IO_ERROR' || result.code === 'SHA512_MISMATCH' ? 1 : 3;
    return undefined;
  }
  process.stderr.write(`usage: ${path.basename(process.argv[1])} check|patch|stage [options]\n`);
  process.exitCode = 2;
  return undefined;
}

if (require.main === module) {
  main().catch((error) => {
    process.stderr.write(`${error.stack ?? error}\n`);
    process.exitCode = 1;
  });
}

module.exports = { extractOfficialResources, cmdStage, parseArgs };
