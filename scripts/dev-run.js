#!/usr/bin/env node
'use strict';

// Development run: patches the cached official release and starts it with
// stock Electron (binary name "electron", so app.isPackaged is false), loading
// the addons straight from native/*/build through the official development
// path <appPath>/../../packages/<pkg>/prebuilds/linux-<arch>/:
//
//   build/dev-<arch>/electron/                      stock Electron + ffmpeg wrapper
//   build/dev-<arch>/out/app.asar                   patched archive (rebuilt each run)
//   build/dev-<arch>/packages/native-recorder/...   symlink to the recorder addon
//   build/dev-<arch>/packages/native-monitor/...    symlink to the audio monitor addon
//
//   node scripts/dev-run.js [-- <app arguments>]
//
// Updates are disabled (as with electron-updater in unpackaged runs) and
// linux-runtime logs go to stderr as well.

const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const common = require('./lib/common');
const { downloadElectron, extractElectron } = require('./lib/electron');
const { fetchOfficial } = require('./fetch-official');
const native = require('./build-native');
const patcher = require('../patcher');
const { computeRuntimeId } = require('../patcher/lib/runtime-id');
const { checkSurface } = require('../patcher/lib/api-surface');
const { templateVars, renderPackagingFile } = require('./build');

const { REPO_ROOT, BUILD_DIR, constants, log } = common;
const DEV_PACKAGES = { 'recorder_linux.node': 'native-recorder', 'audio_monitor_linux.node': 'native-monitor' };

// Electron's default_app (resources/default_app.asar) is what loads an app
// given on the command line, so the development copy keeps it.
async function prepareElectron(dir, arch) {
  const versionFile = path.join(dir, 'version');
  if (fs.existsSync(versionFile) && fs.readFileSync(versionFile, 'utf8').trim() === constants.ELECTRON_VERSION) return;
  common.rmrf(dir);
  const zip = await downloadElectron({ version: constants.ELECTRON_VERSION, arch });
  await extractElectron(zip, dir);
}

async function prepare() {
  const arch = process.arch;
  const root = path.join(BUILD_DIR, `dev-${arch}`);
  const electronDir = path.join(root, 'electron');
  for (const addon of native.ADDONS) {
    if (!fs.existsSync(native.addonPath(addon))) throw new Error(`missing ${native.addonPath(addon)}; run "npm run build:native" first`);
  }
  const nativeApi = native.describeNativeApi();
  const official = await fetchOfficial({});
  await prepareElectron(electronDir, arch);

  const resources = path.join(electronDir, 'resources');
  fs.writeFileSync(path.join(resources, 'ffmpeg'), fs.readFileSync(path.join(REPO_ROOT, 'packaging', 'ffmpeg.sh')), { mode: 0o755 });
  const launcher = path.join(electronDir, 'plaud-linux-dev');
  fs.writeFileSync(launcher, renderPackagingFile('launcher.sh', templateVars({ EXECUTABLE: 'electron' })), { mode: 0o755 });

  const runtimeId = computeRuntimeId({
    patcherDir: path.join(REPO_ROOT, 'patcher'),
    runtimeDir: path.join(REPO_ROOT, 'linux-runtime'),
    nativeApi,
    electronVersion: constants.ELECTRON_VERSION,
  });
  const asar = path.join(root, 'out', 'app.asar');
  fs.mkdirSync(path.dirname(asar), { recursive: true });
  const result = patcher.patchResources({
    resourcesDir: official.resourcesDir,
    outAsar: asar,
    runtimeDir: path.join(REPO_ROOT, 'linux-runtime'),
    runtimeId,
    computeBaseline: true,
  });
  const baseline = { upstreamVersion: result.version, nodeFiles: result.surface.nodeFiles, candidates: result.surface.candidates, used: result.surface.used };
  const sanity = checkSurface({ surface: result.surface, baseline, nativeApi });
  if (!sanity.ok) throw new Error(`the Linux addons do not cover Plaud ${result.version}:\n- ${sanity.problems.join('\n- ')}`);

  for (const addon of native.ADDONS) {
    const link = path.join(root, 'packages', DEV_PACKAGES[addon.file], 'prebuilds', `linux-${arch}`, addon.file);
    fs.mkdirSync(path.dirname(link), { recursive: true });
    fs.rmSync(link, { force: true });
    fs.symlinkSync(native.addonPath(addon), link);
  }
  log('dev', `patched Plaud ${result.version} into ${path.relative(REPO_ROOT, asar)}`);
  return { launcher, asar };
}

async function main(appArgs) {
  const { launcher, asar } = await prepare();
  const child = spawn(launcher, [asar, ...appArgs], {
    stdio: 'inherit',
    env: { ...process.env, PLAUD_LINUX_LOG_STDERR: process.env.PLAUD_LINUX_LOG_STDERR ?? '1' },
  });
  child.on('exit', (code, signal) => {
    process.exitCode = code ?? (signal ? 1 : 0);
  });
}

module.exports = { prepare };

if (require.main === module) {
  const dash = process.argv.indexOf('--');
  main(dash === -1 ? [] : process.argv.slice(dash + 1)).catch((error) => {
    process.stderr.write(`dev-run: ${error.stack ?? error.message}\n`);
    process.exitCode = 1;
  });
}
