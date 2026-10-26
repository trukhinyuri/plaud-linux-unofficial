'use strict';

// "Launch at login" through the XDG Autostart specification:
// $XDG_CONFIG_HOME/autostart/<app id>.desktop, honoured by GNOME, KDE, Xfce,
// Cinnamon, LXQt and by systemd's xdg-autostart generator (uwsm, other
// systemd-managed sessions). The entry passes --auto-launch, which the
// official app reads in isAutoLaunched() to start minimised to the tray.

const fs = require('node:fs');
const path = require('node:path');
const paths = require('./paths');
const constants = require('./constants');
const { createLogger } = require('./log');

const log = createLogger('autostart');

const entryPath = () => path.join(paths.configHome(), 'autostart', `${constants.APP_ID}.desktop`);

/**
 * Command line that starts this installation: the launcher script, AppImage
 * or binary, plus the app path for unpackaged runs (`electron <app>`, as used
 * by scripts/dev-run.js).
 */
function launchCommand() {
  const launcher = process.env.PLAUD_LINUX_LAUNCHER || process.env.APPIMAGE || process.execPath;
  return process.defaultApp && process.argv[1] ? [launcher, path.resolve(process.argv[1])] : [launcher];
}

/** Quotes one argument of a desktop entry Exec key (Desktop Entry spec, "The Exec key"). */
function quoteExecArg(arg) {
  const escapedPercent = arg.replace(/%/g, '%%');
  if (/^[A-Za-z0-9_./@+,:=%-]+$/.test(escapedPercent)) return escapedPercent;
  // Inside double quotes `"`, `` ` ``, `$` and `\` need a backslash; the
  // backslash itself must then be doubled again for the string-level escape.
  const quoted = escapedPercent.replace(/[\\"`$]/g, (c) => `\\\\${c === '\\' ? '\\\\' : c}`);
  return `"${quoted}"`;
}

function entryContent(command) {
  return [
    '[Desktop Entry]',
    'Type=Application',
    `Name=${constants.DISPLAY_NAME}`,
    `Comment=${constants.SUMMARY}`,
    `Exec=${command.map(quoteExecArg).join(' ')} ${constants.AUTO_LAUNCH_ARG}`,
    `Icon=${constants.APP_ID}`,
    'Terminal=false',
    'X-GNOME-Autostart-enabled=true',
    'X-KDE-autostart-after=panel',
    '',
  ].join('\n');
}

function readEntry() {
  try {
    return fs.readFileSync(entryPath(), 'utf8');
  } catch {
    return null;
  }
}

function isEnabledEntry(text) {
  if (text === null) return false;
  return !/^Hidden\s*=\s*true\s*$/m.test(text) && !/^X-GNOME-Autostart-enabled\s*=\s*false\s*$/m.test(text);
}

async function setAutoLaunch(enabled) {
  const file = entryPath();
  if (enabled) {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    const tmp = `${file}.tmp-${process.pid}`;
    fs.writeFileSync(tmp, entryContent(launchCommand()), { mode: 0o644 });
    fs.renameSync(tmp, file);
    log.info('autostart enabled', { file, command: launchCommand() });
  } else {
    fs.rmSync(file, { force: true });
    log.info('autostart disabled', { file });
  }
}

function getAutoLaunch() {
  return isEnabledEntry(readEntry());
}

/** Rewrites an enabled entry whose command no longer matches this installation. */
async function migrateAutoLaunch() {
  const text = readEntry();
  if (!isEnabledEntry(text)) return;
  const wanted = entryContent(launchCommand());
  if (text !== wanted) {
    await setAutoLaunch(true);
    log.info('autostart entry updated to the current launcher');
  }
}

module.exports = { setAutoLaunch, getAutoLaunch, migrateAutoLaunch, entryPath, entryContent, quoteExecArg, launchCommand };
