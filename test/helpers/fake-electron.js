'use strict';

// Minimal stand-in for the `electron` module so linux-runtime modules that
// import it can be unit-tested under plain Node. Only the members the tested
// code touches exist; tests adjust them per case through `fake`.

const Module = require('node:module');
const { EventEmitter } = require('node:events');

const app = Object.assign(new EventEmitter(), {
  isPackaged: true,
  version: '1.4.1',
  getVersion() {
    return this.version;
  },
  commandLine: {
    switches: new Map(),
    hasSwitch(name) {
      return this.switches.has(name);
    },
    getSwitchValue(name) {
      return this.switches.get(name) ?? '';
    },
    appendSwitch(name, value = '') {
      this.switches.set(name, value);
    },
  },
  whenReady: () => Promise.resolve(),
  relaunch() {},
  quit() {},
  exit() {},
});

const fake = {
  app,
  globalShortcut: {
    registered: new Map(),
    register(accelerator, handler) {
      this.registered.set(accelerator, handler);
      return true;
    },
    unregister(accelerator) {
      this.registered.delete(accelerator);
    },
    unregisterAll() {
      this.registered.clear();
    },
    isRegistered(accelerator) {
      return this.registered.has(accelerator);
    },
  },
  screen: {
    displays: [{ id: 1, bounds: { x: 0, y: 0, width: 1920, height: 1080 }, workArea: { x: 0, y: 32, width: 1920, height: 1048 }, scaleFactor: 1 }],
    cursor: { x: 100, y: 100 },
    cursorCalls: 0,
    getAllDisplays() {
      return this.displays;
    },
    getPrimaryDisplay() {
      return this.displays[0];
    },
    getDisplayNearestPoint(point) {
      return (
        this.displays.find(
          (d) => point.x >= d.bounds.x && point.x < d.bounds.x + d.bounds.width && point.y >= d.bounds.y && point.y < d.bounds.y + d.bounds.height,
        ) ?? this.displays[0]
      );
    },
    getDisplayMatching(rect) {
      return this.getDisplayNearestPoint({ x: rect.x + rect.width / 2, y: rect.y + rect.height / 2 });
    },
    getCursorScreenPoint() {
      this.cursorCalls += 1;
      return this.cursor;
    },
  },
  BrowserWindow: class BrowserWindow {
    static windows = [];

    static getAllWindows() {
      return this.windows;
    }

    constructor({ title = '', show = true } = {}) {
      this.title = title;
      this.visible = show;
      this.destroyed = false;
      // Titles the window had each time it was mapped.
      this.mapTitles = [];
    }

    isDestroyed() {
      return this.destroyed;
    }

    isVisible() {
      return this.visible;
    }

    getTitle() {
      return this.title;
    }

    setTitle(title) {
      this.title = title;
    }

    showInactive() {
      if (!this.visible) this.mapTitles.push(this.title);
      this.visible = true;
    }
  },
  systemPreferences: {},
  Menu: { buildFromTemplate: (template) => ({ template }) },
  Notification: class {
    static isSupported() {
      return false;
    }
  },
  shell: { openExternal: async () => {} },
  safeStorage: {
    available: true,
    plain: false,
    isEncryptionAvailable() {
      return this.available;
    },
    setUsePlainTextEncryption(value) {
      this.plain = value;
    },
    getSelectedStorageBackend() {
      return this.available ? 'gnome_libsecret' : 'basic_text';
    },
  },
  net: { fetch: (...args) => fetch(...args) },
};

// The official app's i18next instance, which linux-runtime/tray.js listens to.
const i18next = new EventEmitter();

const originalLoad = Module._load;
Module._load = function load(request, parent, isMain) {
  if (request === 'electron') return fake;
  if (request === 'i18next') return i18next;
  return originalLoad.call(this, request, parent, isMain);
};

module.exports = fake;
