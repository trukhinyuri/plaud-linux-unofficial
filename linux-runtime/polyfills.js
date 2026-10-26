'use strict';

// Electron APIs that the official app calls unconditionally but that do not
// exist, or misbehave, on Linux.

const { app } = require('electron');
const hyprland = require('./hyprland');
const { createLogger } = require('./log');

const log = createLogger('polyfills');

/**
 * systemPreferences.getMediaAccessStatus/askForMediaAccess are macOS/Windows
 * only. The app evaluates getMediaAccessStatus("microphone") while loading a
 * module, so without them the main process fails to start. Linux desktops
 * have no per-app microphone permission for unsandboxed applications; device
 * access is decided by PipeWire/WirePlumber, so "granted" is the accurate
 * answer.
 */
function installEarly() {
  const { systemPreferences } = require('electron');
  if (typeof systemPreferences.getMediaAccessStatus !== 'function') {
    systemPreferences.getMediaAccessStatus = () => 'granted';
    log.info('installed systemPreferences.getMediaAccessStatus');
  }
  if (typeof systemPreferences.askForMediaAccess !== 'function') {
    systemPreferences.askForMediaAccess = async () => true;
  }
}

/** The ozone platform Electron runs on ("x11" or "wayland"). */
function ozonePlatform() {
  const explicit = app.commandLine.getSwitchValue('ozone-platform');
  if (explicit) return explicit;
  const hint = app.commandLine.getSwitchValue('ozone-platform-hint') || process.env.ELECTRON_OZONE_PLATFORM_HINT || '';
  if ((hint === 'auto' || hint === 'wayland') && process.env.WAYLAND_DISPLAY) return 'wayland';
  return 'x11';
}

/**
 * On Wayland, Electron 36's screen.getCursorScreenPoint() blocks the main
 * thread forever when the app has no visible window (observed on Hyprland).
 * The app calls it to choose the display for its tray-less popups (meeting
 * detection tip, recording widget). Only ask Chromium while a window is
 * visible; otherwise answer with the last known position or the centre of
 * the primary display.
 *
 * In Hyprland sessions the pointer position and the work areas come from the
 * compositor (hyprland.js) on both ozone platforms; the answers above remain
 * the fallback when hyprctl fails. Windows shown with showInactive() map under
 * a window rule that keeps Hyprland from focusing them. Must run after 'ready'.
 */
function installAfterReady() {
  const { screen, BrowserWindow } = require('electron');
  const original = screen.getCursorScreenPoint;
  let cursor = () => original.call(screen);
  if (ozonePlatform() === 'wayland') {
    let last = null;
    cursor = () => {
      const visible = BrowserWindow.getAllWindows().some((w) => !w.isDestroyed() && w.isVisible());
      if (visible) {
        last = original.call(screen);
        return last;
      }
      if (last) return last;
      const { bounds } = screen.getPrimaryDisplay();
      return { x: Math.round(bounds.x + bounds.width / 2), y: Math.round(bounds.y + bounds.height / 2) };
    };
    screen.getCursorScreenPoint = cursor;
    log.info('installed Wayland-safe screen.getCursorScreenPoint');
  }
  if (hyprland.inSession()) {
    hyprland.installScreenPolyfill(screen, cursor);
    hyprland.installInactiveShow(BrowserWindow);
  }
}

module.exports = { installEarly, installAfterReady, ozonePlatform };
