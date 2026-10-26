'use strict';

// Placement and focus help from the Hyprland compositor.
//
// The official app places its tray panel and popups from the pointer position
// and the work area of a display. Under XWayland neither is right in a
// Hyprland session: XWayland sees the pointer only while it is over an X11
// window (so it is stale while the pointer rests on a bar or a Wayland
// window), and Hyprland publishes no _NET_WORKAREA, so the work area includes
// the bars. In Hyprland sessions both are read from the compositor with
// hyprctl and converted to Electron's screen coordinates.
//
// The app shows its notification popups (meeting detection tip, generated
// notes, ...) with showInactive(), which must not take the keyboard focus.
// Chromium marks such windows with _NET_WM_USER_TIME 0; Hyprland ignores that
// and focuses every window that maps. A runtime window rule keeps it from
// focusing windows that map under a marker title.

const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const constants = require('./constants');
const { createLogger } = require('./log');

const log = createLogger('hyprland');

// Bars appear and disappear rarely; placement code asks for the displays
// several times per window it shows.
const MONITORS_TTL_MS = 2000;

let monitorCache = { at: 0, list: null };

function inSession() {
  return Boolean(process.env.HYPRLAND_INSTANCE_SIGNATURE && process.env.WAYLAND_DISPLAY);
}

function run(args) {
  return execFileSync('hyprctl', args, { encoding: 'utf8', timeout: 1000, stdio: ['ignore', 'pipe', 'ignore'] });
}

function hyprctl(command) {
  try {
    return JSON.parse(run(['-j', command]));
  } catch (error) {
    log.debug('hyprctl failed', { command, error: error.message });
    return null;
  }
}

/** Enabled monitors (`hyprctl -j monitors`), cached for a short time. */
function monitors() {
  const now = Date.now();
  if (now - monitorCache.at > MONITORS_TTL_MS) {
    const list = hyprctl('monitors');
    monitorCache = { at: now, list: Array.isArray(list) ? list.filter((m) => !m.disabled) : null };
  }
  return monitorCache.list;
}

/** Pixel size of a monitor as laid out: a rotation by 90 or 270 degrees swaps the sides. */
function monitorPixels(m) {
  return m.transform % 2 === 1 ? { width: m.height, height: m.width } : { width: m.width, height: m.height };
}

function displayPixels(d) {
  return { width: Math.round(d.bounds.width * d.scaleFactor), height: Math.round(d.bounds.height * d.scaleFactor) };
}

const near = (a, b) => Math.abs(a - b) <= 2;

/**
 * Pairs Hyprland monitors with Electron displays. XWayland lays the outputs
 * out differently from Hyprland (and in device pixels when Hyprland's
 * xwayland:force_zero_scaling is set), so monitors are matched by pixel size;
 * monitors of equal size are matched in left-to-right, top-to-bottom order.
 */
function pair(monitorList, displays) {
  const ms = [...monitorList].sort((a, b) => a.x - b.x || a.y - b.y);
  const ds = [...displays].sort((a, b) => a.bounds.x - b.bounds.x || a.bounds.y - b.bounds.y);
  const used = new Set();
  const pairs = [];
  for (const monitor of ms) {
    const px = monitorPixels(monitor);
    const display = ds.find((d) => {
      if (used.has(d)) return false;
      const dp = displayPixels(d);
      return near(dp.width, px.width) && near(dp.height, px.height);
    });
    if (!display) continue;
    used.add(display);
    pairs.push({ monitor, display });
  }
  return pairs;
}

/** Converts a point in Hyprland's layout (logical pixels) to Electron screen coordinates. */
function toScreenPoint(point, pairs) {
  for (const { monitor: m, display: d } of pairs) {
    const px = monitorPixels(m);
    const width = px.width / m.scale;
    const height = px.height / m.scale;
    if (point.x < m.x || point.y < m.y || point.x >= m.x + width || point.y >= m.y + height) continue;
    const f = m.scale / d.scaleFactor;
    return { x: Math.round(d.bounds.x + (point.x - m.x) * f), y: Math.round(d.bounds.y + (point.y - m.y) * f) };
  }
  return null;
}

/**
 * The work area of `display` without the edges Hyprland reserves for bars
 * (`reserved` is [left, top, right, bottom] in logical pixels), or null.
 */
function workAreaOf(display, pairs) {
  const p = pairs.find((x) => x.display === display);
  if (!p || !Array.isArray(p.monitor.reserved) || p.monitor.reserved.length !== 4) return null;
  const f = p.monitor.scale / display.scaleFactor;
  const [left, top, right, bottom] = p.monitor.reserved.map((v) => Math.max(0, Math.round(v * f)));
  const b = display.bounds;
  if (left + right >= b.width || top + bottom >= b.height) return null;
  return { x: b.x + left, y: b.y + top, width: b.width - left - right, height: b.height - top - bottom };
}

/** Returns `display` with its work area corrected for Hyprland's bars. */
function withWorkArea(display, pairs) {
  if (!display) return display;
  const area = workAreaOf(display, pairs);
  return area ? { ...display, workArea: area, workAreaSize: { width: area.width, height: area.height } } : display;
}

/**
 * Makes the Electron screen module report Hyprland's work areas and pointer
 * position. Must run after app 'ready'. `fallbackCursor` answers when hyprctl
 * cannot.
 */
function installScreenPolyfill(screen, fallbackCursor) {
  const getAllDisplays = screen.getAllDisplays;
  const raw = () => getAllDisplays.call(screen);
  // Electron returns new display objects on every call; match them by id.
  const corrected = (display) => {
    const list = display && monitors();
    if (!list) return display;
    const displays = raw();
    const same = displays.find((d) => d.id === display.id);
    return same ? withWorkArea(same, pair(list, displays)) : display;
  };
  screen.getAllDisplays = () => {
    const displays = raw();
    const list = monitors();
    if (!list) return displays;
    const pairs = pair(list, displays);
    return displays.map((d) => withWorkArea(d, pairs));
  };
  for (const name of ['getPrimaryDisplay', 'getDisplayNearestPoint', 'getDisplayMatching']) {
    const original = screen[name];
    if (typeof original === 'function') screen[name] = (...args) => corrected(original.apply(screen, args));
  }
  screen.getCursorScreenPoint = () => cursorScreenPoint(raw) ?? fallbackCursor();
  log.info('using Hyprland for pointer position and work areas');
}

/** The pointer position in Electron screen coordinates, or null. */
function cursorScreenPoint(getDisplays) {
  const list = monitors();
  if (!list) return null;
  const pos = hyprctl('cursorpos');
  if (!pos || !Number.isFinite(pos.x) || !Number.isFinite(pos.y)) return null;
  return toScreenPoint(pos, pair(list, getDisplays()));
}

// While showInactive() maps a window it carries this title, which the runtime
// window rule matches. The original title returns once Hyprland has applied
// its rules to the mapped window.
const INACTIVE_TITLE = 'Plaud popup';
const RESTORE_TITLE_MS = 1000;

const regexEscape = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
const luaString = (s) => `"${s.replace(/[\\"]/g, '\\$&')}"`;
const CLASS_PATTERN = `^(${[constants.WM_CLASS, constants.APP_ID].map(regexEscape).join('|')})$`;
const TITLE_PATTERN = `^${regexEscape(INACTIVE_TITLE)}$`;

// Newest syntax first: Lua configuration (Hyprland 0.55+; a named rule is
// replaced, not duplicated), hyprlang with match: props (0.53, 0.54),
// windowrulev2 (older releases).
const RULE_COMMANDS = [
  [
    'eval',
    `hl.window_rule({ name = ${luaString(`${constants.APP_ID}.inactive`)}, match = { class = ${luaString(CLASS_PATTERN)}, title = ${luaString(TITLE_PATTERN)} }, no_initial_focus = true })`,
  ],
  ['keyword', 'windowrule', `match:class ${CLASS_PATTERN}, match:title ${TITLE_PATTERN}, no_initial_focus on`],
  ['keyword', 'windowrulev2', `noinitialfocus, class:${CLASS_PATTERN}, title:${TITLE_PATTERN}`],
];

// null until the rule is added (again after each configuration reload, which
// drops runtime rules); then 'installed' or 'unavailable'.
let rule = null;
let warned = false;
let events = null;

/** Hyprland's event socket (.socket2.sock), in the runtime directory since 0.40, in /tmp before. */
function eventSocketPath() {
  const sig = process.env.HYPRLAND_INSTANCE_SIGNATURE;
  const runtimeDir = process.env.XDG_RUNTIME_DIR || `/run/user/${process.getuid()}`;
  return [path.join(runtimeDir, 'hypr', sig, '.socket2.sock'), path.join('/tmp', 'hypr', sig, '.socket2.sock')].find((p) => fs.existsSync(p)) ?? null;
}

function watchConfigReloads() {
  if (events) return;
  const file = eventSocketPath();
  if (!file) return;
  let buffer = '';
  const socket = net.createConnection(file);
  events = socket;
  socket.unref();
  socket.setEncoding('utf8');
  socket.on('data', (chunk) => {
    buffer += chunk;
    for (let i = buffer.indexOf('\n'); i !== -1; i = buffer.indexOf('\n')) {
      if (buffer.startsWith('configreloaded>>')) rule = null;
      buffer = buffer.slice(i + 1);
    }
  });
  socket.on('error', (error) => log.debug('Hyprland event socket failed', { error: error.message }));
  socket.on('close', () => {
    if (events === socket) events = null;
    rule = null;
  });
}

/** Adds the window rule that keeps Hyprland from focusing windows mapped under INACTIVE_TITLE. */
function ensureInactiveRule() {
  // Without the event socket a configuration reload goes unnoticed; add the
  // rule every time (the Lua form replaces itself).
  if (rule !== null && events) return rule === 'installed';
  rule = 'unavailable';
  for (const args of RULE_COMMANDS) {
    try {
      if (run(args).trim() === 'ok') {
        rule = 'installed';
        log.info('window rule for inactive popups added', { syntax: args[0] === 'eval' ? 'lua' : args[1] });
        break;
      }
    } catch (error) {
      log.debug('window rule syntax rejected', { syntax: args[0] === 'eval' ? 'lua' : args[1], error: error.message });
    }
  }
  if (rule === 'unavailable' && !warned) {
    warned = true;
    log.warn('Hyprland accepted no window rule syntax; popups shown inactive will take the keyboard focus');
  }
  watchConfigReloads();
  return rule === 'installed';
}

/**
 * Makes BrowserWindow#showInactive() map windows under INACTIVE_TITLE, so the
 * window rule keeps Hyprland from focusing them, as macOS and Windows do.
 */
function installInactiveShow(BrowserWindow) {
  const showInactive = BrowserWindow.prototype.showInactive;
  BrowserWindow.prototype.showInactive = function showInactiveWithoutFocus(...args) {
    if (this.isDestroyed() || this.isVisible() || !ensureInactiveRule()) return showInactive.apply(this, args);
    const title = this.getTitle();
    this.setTitle(INACTIVE_TITLE);
    try {
      return showInactive.apply(this, args);
    } finally {
      const restore = setTimeout(() => {
        if (!this.isDestroyed() && this.getTitle() === INACTIVE_TITLE) this.setTitle(title);
      }, RESTORE_TITLE_MS);
      restore.unref?.();
    }
  };
}

/** Drops cached state (tests). */
function reset() {
  monitorCache = { at: 0, list: null };
  rule = null;
  warned = false;
  events?.destroy();
  events = null;
}

module.exports = {
  inSession,
  monitors,
  pair,
  toScreenPoint,
  workAreaOf,
  withWorkArea,
  installScreenPolyfill,
  cursorScreenPoint,
  ensureInactiveRule,
  installInactiveShow,
  INACTIVE_TITLE,
  RULE_COMMANDS,
  reset,
};
