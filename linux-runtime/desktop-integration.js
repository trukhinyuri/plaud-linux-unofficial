'use strict';

// Desktop integration for the AppImage.
//
// Signing in completes through a plaud:// link opened by the browser, which
// only reaches the app if a desktop entry declares the URL scheme. Packages
// and scripts/install ship that entry; an AppImage has to install it itself.
// On every start from an AppImage this module writes (or updates) a desktop
// entry pointing at $APPIMAGE plus the hicolor icons into $XDG_DATA_HOME and
// registers the URL scheme. Set PLAUD_LINUX_NO_DESKTOP_INTEGRATION=1 to opt
// out; run the AppImage with --plaud-linux-remove-desktop-integration to undo.

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const paths = require('./paths');
const constants = require('./constants');
const { quoteExecArg } = require('./autostart');
const { createLogger } = require('./log');

const log = createLogger('desktop');
const MARKER = 'X-PlaudLinux-AppImage=true';

const desktopFile = () => path.join(paths.dataHome(), 'applications', `${constants.APP_ID}.desktop`);
const iconsRoot = () => path.join(paths.dataHome(), 'icons', 'hicolor');

function writeIfChanged(file, content) {
  try {
    if (fs.readFileSync(file).equals(Buffer.from(content))) return false;
  } catch {
    // missing: write it
  }
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const tmp = `${file}.tmp-${process.pid}`;
  fs.writeFileSync(tmp, content, { mode: 0o644 });
  fs.renameSync(tmp, file);
  return true;
}

function run(cmd, args) {
  const res = spawnSync(cmd, args, { stdio: 'ignore', timeout: 10_000 });
  return !res.error && res.status === 0;
}

function bundledDesktopEntry(appDir) {
  return fs.readFileSync(path.join(appDir, `${constants.APP_ID}.desktop`), 'utf8');
}

function ensure() {
  const appImage = process.env.APPIMAGE;
  const appDir = process.env.APPDIR;
  if (!appImage || !appDir || process.env.PLAUD_LINUX_NO_DESKTOP_INTEGRATION === '1') return;
  try {
    const existing = fs.existsSync(desktopFile()) ? fs.readFileSync(desktopFile(), 'utf8') : null;
    if (existing !== null && !existing.includes(MARKER)) {
      log.info('a desktop entry from a package or scripts/install exists; not replacing it', { file: desktopFile() });
      return;
    }
    const entry = bundledDesktopEntry(appDir)
      .replace(/^Exec=.*$/m, `Exec=${quoteExecArg(appImage)} %u`)
      .replace(/^TryExec=.*\n/m, '')
      .replace(/\n*$/, `\n${MARKER}\n`);
    let changed = writeIfChanged(desktopFile(), entry);
    const sourceIcons = path.join(appDir, 'usr', 'share', 'icons', 'hicolor');
    for (const size of fs.readdirSync(sourceIcons)) {
      const src = path.join(sourceIcons, size, 'apps', `${constants.APP_ID}.png`);
      if (!fs.existsSync(src)) continue;
      changed = writeIfChanged(path.join(iconsRoot(), size, 'apps', `${constants.APP_ID}.png`), fs.readFileSync(src)) || changed;
    }
    if (changed) {
      run('update-desktop-database', [path.dirname(desktopFile())]);
      run('gtk-update-icon-cache', ['--quiet', '--ignore-theme-index', iconsRoot()]);
      log.info('desktop integration installed', { file: desktopFile(), appImage });
    }
    run('xdg-mime', ['default', `${constants.APP_ID}.desktop`, `x-scheme-handler/${constants.PROTOCOL}`]);
  } catch (error) {
    log.error('desktop integration failed', { error });
  }
}

function remove() {
  const file = desktopFile();
  if (fs.existsSync(file) && fs.readFileSync(file, 'utf8').includes(MARKER)) {
    fs.rmSync(file, { force: true });
    try {
      for (const size of fs.readdirSync(iconsRoot())) {
        fs.rmSync(path.join(iconsRoot(), size, 'apps', `${constants.APP_ID}.png`), { force: true });
      }
    } catch {
      // no icons installed
    }
    run('update-desktop-database', [path.dirname(file)]);
    log.info('desktop integration removed', { file });
    process.stdout.write(`removed ${file}\n`);
  } else {
    process.stdout.write('no AppImage desktop integration found\n');
  }
}

module.exports = { ensure, remove, MARKER };
