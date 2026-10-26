'use strict';

// Tray integration for StatusNotifierItem (KDE, GNOME with the AppIndicator
// extension, Waybar, ...) and XEmbed trays.

const path = require('node:path');
const { app, BrowserWindow, Menu, screen } = require('electron');
const hyprland = require('./hyprland');
const { createLogger } = require('./log');

const log = createLogger('tray');

// Official icon type → PNG rendered by the patcher from resources/tray/*.ico.
const ICONS = { off: 'Icon-off.png', on: 'Icon-on.png', 'ppc-off': 'Icon-ppc.png', 'update-available': 'Icon-update.png' };

/** Path of the Linux tray icon for an official icon type. */
function iconPath(type = 'off') {
  const file = ICONS[type] ?? ICONS.off;
  return path.join(__dirname, '..', 'resources', 'tray', 'linux', file);
}

function signature(template) {
  return JSON.stringify(
    template.map((item) => [item.id, item.type, item.label, item.visible, item.enabled, item.checked]),
  );
}

/**
 * The official app shows its tray menu from the "right-click" event with
 * popUpContextMenu(); StatusNotifierItem hosts own the menu instead, so it is
 * exported with setContextMenu(). The menu content depends on the UI language
 * and the signed-in state, so it is rebuilt whenever either may have changed.
 */
function attachContextMenu(tray, getMenu) {
  let current = null;
  let timer = null;
  const refresh = (reason) => {
    if (tray.isDestroyed()) {
      clearInterval(timer);
      return;
    }
    let template;
    try {
      template = getMenu();
    } catch (error) {
      log.error('building the tray menu failed', { error });
      return;
    }
    const sig = signature(template);
    if (sig === current) return;
    current = sig;
    for (const item of template) {
      if (typeof item.click !== 'function') continue;
      const click = item.click;
      // Sign-in, sign-out and language changes alter the menu itself.
      item.click = (...args) => {
        click(...args);
        for (const delay of [1000, 5000]) setTimeout(() => refresh('menu action'), delay).unref?.();
      };
    }
    tray.setContextMenu(Menu.buildFromTemplate(template));
    log.debug('tray menu updated', { reason });
  };
  refresh('init');

  const i18next = require('i18next');
  i18next.on('languageChanged', () => refresh('language changed'));
  for (const event of ['browser-window-focus', 'browser-window-blur', 'browser-window-created']) {
    app.on(event, () => refresh(event));
  }
  tray.on('click', () => {
    rememberClickPoint();
    togglePanel();
    refresh('tray click');
  });
  // Catch state changes that happen without any window activity
  // (for example a token expiring in the background).
  timer = setInterval(() => refresh('periodic'), 30_000);
  timer.unref?.();
}

// Where the pointer was at the last tray click, in screen coordinates.
let clickPoint = null;

/**
 * Whether screen.getCursorScreenPoint() is right while the pointer is on a
 * panel: in X11 sessions, and in Hyprland sessions where it is read from the
 * compositor (hyprland.js). XWayland and Wayland clients elsewhere do not see
 * the pointer over panels.
 */
function pointerKnown() {
  return (process.env.XDG_SESSION_TYPE === 'x11' && !process.env.WAYLAND_DISPLAY) || hyprland.inSession();
}

function rememberClickPoint() {
  if (!pointerKnown()) return;
  try {
    clickPoint = screen.getCursorScreenPoint();
  } catch (error) {
    log.warn('reading the pointer position failed', { error });
  }
}

/**
 * The official tray click handler toggles the tray panel on Windows by
 * relying on the click taking the focus away from the panel: the panel hides
 * on blur, and a click within 300 ms of that blur leaves it hidden
 * (index-C7esC3cC.js:347-355, 752-778). Clicking a StatusNotifierItem does not
 * move the focus, so the click would show the open panel again. Deliver the
 * blur the panel would have received. This listener runs before the official
 * one because the tray-context-menu patch installs it before the official
 * click handler is registered.
 */
function togglePanel() {
  const panel = BrowserWindow.getAllWindows().find(
    (w) => !w.isDestroyed() && w.webContents.getURL().includes('/window/recording-main/'),
  );
  if (panel?.isVisible()) panel.emit('blur');
}

function panelEdge(display) {
  const { bounds, workArea } = display;
  if (workArea.y > bounds.y) return 'top';
  if (workArea.y + workArea.height < bounds.y + bounds.height) return 'bottom';
  if (workArea.x > bounds.x) return 'left';
  if (workArea.x + workArea.width < bounds.x + bounds.width) return 'right';
  return null;
}

const SIZE = 24;

/**
 * Tray icon bounds around `point` (the pointer at a tray click), moved onto
 * the inner edge of the panel that holds the tray, so that the official code
 * places the tray panel next to the bar instead of over it.
 */
function boundsAtPoint(point) {
  const display = screen.getDisplayNearestPoint(point);
  const { workArea } = display;
  let x = point.x - SIZE / 2;
  let y = point.y - SIZE / 2;
  switch (panelEdge(display)) {
    case 'top':
      y = workArea.y - SIZE;
      break;
    case 'bottom':
      y = workArea.y + workArea.height;
      break;
    case 'left':
      x = workArea.x;
      break;
    case 'right':
      x = workArea.x + workArea.width - SIZE;
      break;
  }
  return { x: Math.round(x), y: Math.round(y), width: SIZE, height: SIZE };
}

/**
 * Bounds of a tray that has not been clicked yet: a corner of the work area
 * on the display under the pointer, on the side where a panel reserves space
 * (vertical panels keep their tray at the bottom), else the top-right corner.
 */
function boundsInCorner() {
  const display = screen.getDisplayNearestPoint(screen.getCursorScreenPoint());
  const { workArea } = display;
  const margin = 8;
  const right = workArea.x + workArea.width - SIZE;
  const bottom = workArea.y + workArea.height - SIZE;
  switch (panelEdge(display)) {
    case 'top':
      return { x: right - margin, y: workArea.y - SIZE, width: SIZE, height: SIZE };
    case 'bottom':
      return { x: right - margin, y: workArea.y + workArea.height, width: SIZE, height: SIZE };
    case 'left':
      return { x: workArea.x, y: bottom, width: SIZE, height: SIZE };
    case 'right':
      return { x: right, y: bottom, width: SIZE, height: SIZE };
    default:
      return { x: right - margin, y: workArea.y, width: SIZE, height: SIZE };
  }
}

/**
 * Tray.getBounds() is always empty on Linux. The official code anchors its
 * tray panel to these bounds, so return the position of the last tray click
 * where the pointer position is known (X11 and Hyprland sessions), else a
 * work-area corner.
 */
function getBounds() {
  return clickPoint ? boundsAtPoint(clickPoint) : boundsInCorner();
}

module.exports = { iconPath, attachContextMenu, getBounds };
