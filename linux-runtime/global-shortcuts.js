'use strict';

// Global shortcuts for the recording actions (highlight, screenshot).
//
// Electron registers global shortcuts with X11 key grabs. In a Wayland
// session the app runs through XWayland by default, where a grab only fires
// while an XWayland window has focus. The XDG GlobalShortcuts portal is the
// Wayland mechanism (KDE Plasma, GNOME 48+, Hyprland and others), so in
// Wayland sessions the shortcuts are bound through the portal in addition to
// Electron. The app throttles both handlers (500 ms), so a key press that
// reaches both paths triggers the action once.
//
// The portal binding (and, on GNOME/KDE, the confirmation dialog) happens the
// first time the app registers its shortcuts; the bound set is remembered so
// later sessions bind at start-up without waiting for a recording.

const { app, globalShortcut } = require('electron');
const { DBusConnection } = require('./dbus');
const constants = require('./constants');
const { readState, writeState } = require('./staging');
const { createLogger } = require('./log');

const log = createLogger('shortcuts');

const PORTAL = { destination: 'org.freedesktop.portal.Desktop', path: '/org/freedesktop/portal/desktop' };
const GS = 'org.freedesktop.portal.GlobalShortcuts';
const DESCRIPTIONS = {
  highlight: 'Plaud: highlight the current moment of the recording',
  screenshot: 'Plaud: add a screenshot to the recording',
};
const STATE_FILE = 'portal-shortcuts.json';

const MODIFIERS = {
  alt: 'ALT', option: 'ALT', shift: 'SHIFT', ctrl: 'CTRL', control: 'CTRL', commandorcontrol: 'CTRL', cmdorctrl: 'CTRL',
  super: 'LOGO', meta: 'LOGO', cmd: 'LOGO', command: 'LOGO',
};
const KEYS = {
  space: 'space', enter: 'Return', return: 'Return', tab: 'Tab', backspace: 'BackSpace', delete: 'Delete', escape: 'Escape', esc: 'Escape',
  up: 'Up', down: 'Down', left: 'Left', right: 'Right', home: 'Home', end: 'End', pageup: 'Page_Up', pagedown: 'Page_Down', insert: 'Insert',
  plus: 'plus', '-': 'minus', '=': 'equal', ',': 'comma', '.': 'period', '/': 'slash', ';': 'semicolon', "'": 'apostrophe',
  '[': 'bracketleft', ']': 'bracketright', '\\': 'backslash', '`': 'grave',
};

/** Converts an Electron accelerator into an XDG shortcut trigger ("ALT+SHIFT+h"), or null. */
function toPortalTrigger(accelerator) {
  const parts = String(accelerator).split('+').filter(Boolean);
  if (parts.length === 0) return null;
  const key = parts.pop();
  const mods = [];
  for (const p of parts) {
    const m = MODIFIERS[p.toLowerCase()];
    if (!m) return null;
    if (!mods.includes(m)) mods.push(m);
  }
  let sym;
  if (/^[A-Za-z]$/.test(key)) sym = key.toLowerCase();
  else if (/^[0-9]$/.test(key)) sym = key;
  else if (/^F([1-9]|1[0-9]|2[0-4])$/i.test(key)) sym = key.toUpperCase();
  else sym = KEYS[key.toLowerCase()] ?? null;
  if (!sym) return null;
  return [...mods, sym].join('+');
}

function isWaylandSession() {
  return Boolean(process.env.WAYLAND_DISPLAY) || process.env.XDG_SESSION_TYPE === 'wayland';
}

function portalWanted() {
  const mode = process.env.PLAUD_LINUX_SHORTCUTS || 'auto';
  if (mode === 'electron') return false;
  if (mode === 'portal') return true;
  return isWaylandSession();
}

const handlers = new Map(); // accelerator -> { id, handler }
const bindings = new Map(); // shortcut id -> accelerator (what the portal should hold)
let portal = null; // { conn, session, bound: Map }
let syncTimer = null;
let syncing = Promise.resolve();
let portalUnavailable = false;

function token() {
  return `plaud${process.pid}_${Math.random().toString(36).slice(2, 10)}`;
}

/** Calls a portal method returning a Request and waits for its Response. */
async function request(conn, member, signature, body, optionsIndex) {
  const handleToken = token();
  const sender = conn.uniqueName.slice(1).replace(/\./g, '_');
  const requestPath = `/org/freedesktop/portal/desktop/request/${sender}/${handleToken}`;
  let settle;
  const response = new Promise((resolve) => (settle = resolve));
  const unsubscribe = await conn.subscribe(
    { interface: 'org.freedesktop.portal.Request', member: 'Response', path: requestPath },
    (msg) => settle(msg.body),
  );
  try {
    body[optionsIndex] = { ...body[optionsIndex], handle_token: ['s', handleToken] };
    await conn.call({ ...PORTAL, interface: GS, member, signature, body });
    const [code, results] = await response;
    if (code !== 0) throw new Error(`${member} was ${code === 1 ? 'cancelled by the user' : 'refused by the portal'}`);
    return results;
  } finally {
    await unsubscribe();
  }
}

async function openPortal() {
  const conn = await DBusConnection.connect();
  conn.on('close', () => {
    if (portal?.conn === conn) portal = null;
  });
  try {
    // Associates this unsandboxed connection with our application id
    // (xdg-desktop-portal >= 1.19); older portals do not have the interface.
    await conn.call({ ...PORTAL, interface: 'org.freedesktop.host.portal.Registry', member: 'Register', signature: 'sa{sv}', body: [constants.APP_ID, {}] });
  } catch (error) {
    log.info('portal application registry unavailable', { error: error.message });
  }
  const [version] = await conn.call({ ...PORTAL, interface: 'org.freedesktop.DBus.Properties', member: 'Get', signature: 'ss', body: [GS, 'version'] });
  const results = await request(conn, 'CreateSession', 'a{sv}', [{ session_handle_token: ['s', token()] }], 0);
  const session = results.session_handle;
  await conn.subscribe({ interface: GS, member: 'Activated', path: PORTAL.path }, (msg) => {
    const [sessionHandle, shortcutId] = msg.body;
    if (sessionHandle !== session) return;
    const accelerator = bindings.get(shortcutId);
    const entry = accelerator && handlers.get(accelerator);
    log.info('portal shortcut activated', { shortcutId, active: Boolean(entry) });
    if (entry) entry.handler();
  });
  log.info('portal session created', { version, session });
  return { conn, session, bound: new Map() };
}

async function closePortal() {
  if (!portal) return;
  const { conn, session } = portal;
  portal = null;
  await conn.call({ destination: PORTAL.destination, path: session, interface: 'org.freedesktop.portal.Session', member: 'Close' }).catch(() => {});
  conn.close();
}

function sameBindings(a, b) {
  return a.size === b.size && [...a].every(([id, acc]) => b.get(id) === acc);
}

async function syncPortal() {
  if (portalUnavailable || bindings.size === 0) return;
  if (portal && sameBindings(portal.bound, bindings)) return;
  // A session's shortcuts are bound once; a changed set gets a new session.
  await closePortal();
  try {
    portal = await openPortal();
  } catch (error) {
    portalUnavailable = true;
    log.warn('GlobalShortcuts portal unavailable; shortcuts only work while a Plaud window has focus', { error: error.message });
    return;
  }
  const wanted = new Map(bindings);
  const shortcuts = [];
  for (const [id, accelerator] of wanted) {
    const options = { description: ['s', DESCRIPTIONS[id] ?? `Plaud: ${id}`] };
    const trigger = toPortalTrigger(accelerator);
    if (trigger) options.preferred_trigger = ['s', trigger];
    shortcuts.push([id, options]);
  }
  let results;
  try {
    results = await request(portal.conn, 'BindShortcuts', 'oa(sa{sv})sa{sv}', [portal.session, shortcuts, '', {}], 3);
  } catch (error) {
    log.warn('binding portal shortcuts failed', { error: error.message });
    await closePortal();
    return;
  }
  portal.bound = wanted;
  const bound = (results.shortcuts ?? []).map(([id, props]) => ({ id, trigger: props.trigger_description }));
  log.info('portal shortcuts bound', { bound });
  try {
    writeState(STATE_FILE, Object.fromEntries(wanted));
  } catch (error) {
    log.warn('remembering the portal shortcuts failed', { error: error.message });
  }
}

function scheduleSync() {
  if (!portalWanted()) return;
  clearTimeout(syncTimer);
  // Debounced: the app probes for conflicts by registering and immediately
  // unregistering, which must not cause portal traffic.
  syncTimer = setTimeout(() => {
    syncing = syncing.then(syncPortal).catch((error) => log.error('portal sync failed', { error }));
  }, 300);
  syncTimer.unref?.();
}

/**
 * Registers a recording shortcut (called from the patched shortcutService
 * with the action id, e.g. "highlight"). Returns whether the shortcut can
 * fire, as electron.globalShortcut.register() does.
 */
function register(actionId, accelerator, handler) {
  const electronOk = globalShortcut.register(accelerator, handler);
  if (!portalWanted()) return electronOk;
  handlers.set(accelerator, { id: actionId, handler });
  if (bindings.get(actionId) !== accelerator) {
    for (const [id, acc] of bindings) if (acc === accelerator && id !== actionId) bindings.delete(id);
    bindings.set(actionId, accelerator);
  }
  scheduleSync();
  return electronOk || !portalUnavailable;
}

/**
 * Wraps globalShortcut.unregister/unregisterAll/isRegistered so the official
 * code also releases portal-backed shortcuts. Portal bindings stay in place
 * (rebinding would ask the user again); activations of shortcuts the app has
 * not registered are ignored.
 */
function install() {
  const original = {
    unregister: globalShortcut.unregister.bind(globalShortcut),
    unregisterAll: globalShortcut.unregisterAll.bind(globalShortcut),
    isRegistered: globalShortcut.isRegistered.bind(globalShortcut),
  };
  globalShortcut.unregister = (accelerator) => {
    handlers.delete(accelerator);
    return original.unregister(accelerator);
  };
  globalShortcut.unregisterAll = () => {
    handlers.clear();
    return original.unregisterAll();
  };
  globalShortcut.isRegistered = (accelerator) => original.isRegistered(accelerator) || handlers.has(accelerator);

  if (!portalWanted()) return;
  // Re-create the bindings of the previous run so the first recording does
  // not wait for the portal.
  let remembered = null;
  try {
    remembered = readState(STATE_FILE, null);
  } catch (error) {
    log.warn('reading the remembered portal shortcuts failed', { error: error.message });
  }
  if (remembered && typeof remembered === 'object') {
    for (const [id, accelerator] of Object.entries(remembered)) {
      if (typeof accelerator === 'string') bindings.set(id, accelerator);
    }
    app.whenReady().then(scheduleSync);
  }
  app.on('will-quit', () => {
    void closePortal();
  });
}

module.exports = { register, install, toPortalTrigger, portalWanted, _state: { handlers, bindings } };
