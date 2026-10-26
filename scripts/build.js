#!/usr/bin/env node
'use strict';

// Assembles a runnable Linux app directory for one architecture:
//
//   build/linux-<arch>/app/     stock Electron + patched app.asar + addons
//   build/linux-<arch>/share/   desktop entry, icons, AppStream metadata
//
//   node scripts/build.js [--arch x64|arm64] [--latest] [--official-zip <zip>]
//                         [--electron-zip <zip>] [--skip-native-build]
//
// The official release is downloaded and verified (scripts/fetch-official.js),
// its app.asar is patched (patcher/), and the result is combined with stock
// Electron and the Linux addons built from native/.

const fs = require('node:fs');
const path = require('node:path');
const common = require('./lib/common');
const { downloadElectron, extractElectron } = require('./lib/electron');
const { fetchOfficial } = require('./fetch-official');
const native = require('./build-native');
const patcher = require('../patcher');
const { computeRuntimeId } = require('../patcher/lib/runtime-id');
const { checkSurface } = require('../patcher/lib/api-surface');
const { decodeIcns, renderPng } = require('../patcher/lib/icons');

const { REPO_ROOT, BUILD_DIR, constants, log } = common;
const ICON_SIZES = [16, 22, 24, 32, 48, 64, 128, 256, 512];

function buildDate() {
  const epoch = Number(process.env.SOURCE_DATE_EPOCH);
  return (Number.isFinite(epoch) && epoch > 0 ? new Date(epoch * 1000) : new Date()).toISOString().slice(0, 10);
}

function templateVars(extra = {}) {
  return {
    APP_ID: constants.APP_ID,
    DISPLAY_NAME: constants.DISPLAY_NAME,
    GENERIC_NAME: constants.GENERIC_NAME,
    SUMMARY: constants.SUMMARY,
    PROTOCOL: constants.PROTOCOL,
    WM_CLASS: constants.WM_CLASS,
    COMMAND: constants.COMMAND,
    EXECUTABLE: constants.EXECUTABLE,
    PROJECT_URL: constants.PROJECT_URL,
    MAINTAINER: `${constants.MAINTAINER_NAME} <${constants.MAINTAINER_EMAIL}>`,
    MAINTAINER_NAME: constants.MAINTAINER_NAME,
    MAINTAINER_EMAIL: constants.MAINTAINER_EMAIL,
    VERSION: common.repoVersion(),
    DATE: buildDate(),
    ...extra,
  };
}

function renderPackagingFile(name, vars) {
  return common.renderTemplate(fs.readFileSync(path.join(REPO_ROOT, 'packaging', name), 'utf8'), vars);
}

/** Copies the files the in-app updater needs into resources/plaud-linux/. */
function installPatcherCopy(destDir) {
  const copyJs = (srcDir, dst) => {
    fs.mkdirSync(dst, { recursive: true });
    for (const entry of fs.readdirSync(srcDir, { withFileTypes: true })) {
      const src = path.join(srcDir, entry.name);
      if (entry.isDirectory()) copyJs(src, path.join(dst, entry.name));
      else if (/\.(js|json)$/.test(entry.name) && entry.name !== 'patch-manifest.json') fs.copyFileSync(src, path.join(dst, entry.name));
    }
  };
  copyJs(path.join(REPO_ROOT, 'patcher'), path.join(destDir, 'patcher'));
  copyJs(path.join(REPO_ROOT, 'linux-runtime'), path.join(destDir, 'linux-runtime'));
}

function writeShare(shareDir, resourcesDir) {
  common.rmrf(shareDir);
  const frames = decodeIcns(fs.readFileSync(path.join(resourcesDir, 'icon.icns')));
  for (const size of ICON_SIZES) {
    const file = path.join(shareDir, 'icons', 'hicolor', `${size}x${size}`, 'apps', `${constants.APP_ID}.png`);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, renderPng(frames, size));
  }
  const vars = templateVars({ EXEC: constants.COMMAND });
  const desktop = path.join(shareDir, 'applications', `${constants.APP_ID}.desktop`);
  fs.mkdirSync(path.dirname(desktop), { recursive: true });
  fs.writeFileSync(desktop, renderPackagingFile('app.desktop.in', vars));
  const metainfo = path.join(shareDir, 'metainfo', `${constants.APP_ID}.metainfo.xml`);
  fs.mkdirSync(path.dirname(metainfo), { recursive: true });
  fs.writeFileSync(metainfo, renderPackagingFile('metainfo.xml.in', vars));
}

async function build({ arch, latest = false, officialZip, electronZip: localElectronZip, skipNativeBuild = false } = {}) {
  arch = common.resolveArch(arch);
  if (arch !== process.arch) {
    throw new Error(`the native addons are built for the host; cross-building ${arch} on ${process.arch} is not supported`);
  }
  const outDir = path.join(BUILD_DIR, `linux-${arch}`);
  const appDir = path.join(outDir, 'app');
  const resources = path.join(appDir, 'resources');

  // 1. Native addons and their API.
  if (!skipNativeBuild) for (const addon of native.ADDONS) native.buildAddon(addon);
  for (const addon of native.ADDONS) {
    if (!fs.existsSync(native.addonPath(addon))) throw new Error(`missing ${native.addonPath(addon)}; run without --skip-native-build`);
  }
  const nativeApi = native.describeNativeApi();

  // 2. Official release.
  const official = await fetchOfficial({ latest, zip: officialZip });

  // 3. Stock Electron.
  const electronZip = await downloadElectron({ version: constants.ELECTRON_VERSION, arch, localZip: localElectronZip });
  const electron = await extractElectron(electronZip, appDir);
  if (electron.version !== constants.ELECTRON_VERSION) throw new Error(`unexpected Electron version ${electron.version}`);
  fs.renameSync(electron.binary, path.join(appDir, constants.EXECUTABLE));
  fs.rmSync(path.join(resources, 'default_app.asar'), { force: true });

  // 4. Patch the official archive.
  const runtimeId = computeRuntimeId({
    patcherDir: path.join(REPO_ROOT, 'patcher'),
    runtimeDir: path.join(REPO_ROOT, 'linux-runtime'),
    nativeApi,
    electronVersion: constants.ELECTRON_VERSION,
  });
  const result = patcher.patchResources({
    resourcesDir: official.resourcesDir,
    outAsar: path.join(resources, 'app.asar'),
    runtimeDir: path.join(REPO_ROOT, 'linux-runtime'),
    runtimeId,
    computeBaseline: true,
  });
  const baseline = {
    upstreamVersion: result.version,
    nodeFiles: result.surface.nodeFiles,
    candidates: result.surface.candidates,
    used: result.surface.used,
  };
  const sanity = checkSurface({ surface: result.surface, baseline, nativeApi });
  if (!sanity.ok) throw new Error(`the Linux addons do not cover Plaud ${result.version}:\n- ${sanity.problems.join('\n- ')}`);
  log('build', `patched Plaud ${result.version} (${result.manifest.patches.length} patches, runtime ${runtimeId})`);

  // 5. Addons, ffmpeg wrapper, launcher.
  for (const addon of native.ADDONS) fs.copyFileSync(native.addonPath(addon), path.join(resources, addon.file));
  fs.writeFileSync(path.join(resources, 'ffmpeg'), fs.readFileSync(path.join(REPO_ROOT, 'packaging', 'ffmpeg.sh')), { mode: 0o755 });
  const launcher = path.join(appDir, constants.COMMAND);
  fs.writeFileSync(launcher, renderPackagingFile('launcher.sh', templateVars()), { mode: 0o755 });

  // 6. Metadata and updater support.
  const meta = path.join(resources, constants.RESOURCES_SUBDIR);
  installPatcherCopy(meta);
  const buildInfo = {
    name: constants.PACKAGE_NAME,
    version: common.repoVersion(),
    upstreamVersion: result.version,
    upstreamSha512: official.sha512,
    electron: constants.ELECTRON_VERSION,
    arch,
    runtimeId,
  };
  const writeJson = (file, value) => fs.writeFileSync(path.join(meta, file), `${JSON.stringify(value, null, 2)}\n`);
  writeJson('build-info.json', buildInfo);
  writeJson('native-api.json', nativeApi);
  writeJson('api-baseline.json', baseline);
  for (const file of ['LICENSE', 'NOTICE']) {
    if (fs.existsSync(path.join(REPO_ROOT, file))) fs.copyFileSync(path.join(REPO_ROOT, file), path.join(meta, file));
  }

  // 7. Desktop integration files.
  writeShare(path.join(outDir, 'share'), official.resourcesDir);
  log('build', `app directory ready: ${path.relative(REPO_ROOT, appDir)} (run ./${path.relative(REPO_ROOT, launcher)})`);
  return { arch, outDir, appDir, shareDir: path.join(outDir, 'share'), buildInfo };
}

module.exports = { build, writeShare, templateVars, renderPackagingFile, ICON_SIZES };

if (require.main === module) {
  const args = common.parseArgs(process.argv.slice(2), { booleans: ['latest', 'skip-native-build'] });
  build({
    arch: typeof args.arch === 'string' ? args.arch : undefined,
    latest: Boolean(args.latest),
    officialZip: typeof args['official-zip'] === 'string' ? args['official-zip'] : undefined,
    electronZip: typeof args['electron-zip'] === 'string' ? path.resolve(args['electron-zip']) : undefined,
    skipNativeBuild: Boolean(args['skip-native-build']),
  }).catch((error) => {
    process.stderr.write(`build: ${error.stack ?? error.message}\n`);
    process.exitCode = 1;
  });
}
