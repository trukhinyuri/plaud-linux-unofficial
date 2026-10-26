'use strict';

// Identity of the unofficial Linux build. This file is the single source of
// truth for names shared by the runtime injected into the app, the patcher
// and the build/packaging scripts.

module.exports = Object.freeze({
  // Reverse-DNS application id: desktop file id, Wayland app_id, AppStream id,
  // and the id registered with xdg-desktop-portal.
  APP_ID: 'io.github.trukhinyuri.PlaudLinux',
  // Name shown in menus and window title bars: the official product name.
  // The unofficial status is stated in the summary (desktop entry Comment,
  // AppStream summary and description), the package name and the README.
  DISPLAY_NAME: 'Plaud',
  GENERIC_NAME: 'AI meeting recorder',
  SUMMARY: 'Unofficial Linux build of the Plaud Desktop app',
  // The official app calls app.setName(productName); X11 WM_CLASS is derived
  // from it, so StartupWMClass must match.
  WM_CLASS: 'Plaud',
  // Distribution package and launcher command names.
  PACKAGE_NAME: 'plaud-linux-unofficial',
  COMMAND: 'plaud-linux',
  // Name of the renamed Electron executable inside the app directory.
  EXECUTABLE: 'plaud-linux-bin',
  // $XDG_DATA_HOME/<DATA_DIR_NAME>, $XDG_STATE_HOME/<DATA_DIR_NAME>.
  DATA_DIR_NAME: 'plaud-linux',
  // Sub-directory of process.resourcesPath with build metadata, the bundled
  // patcher and the runtime sources (used by the in-app updater).
  RESOURCES_SUBDIR: 'plaud-linux',
  // URL scheme handled by the official app (regionConfig.protocols).
  PROTOCOL: 'plaud',
  // Command line flag the official app checks in isAutoLaunched().
  AUTO_LAUNCH_ARG: '--auto-launch',
  // Official electron-updater generic feed (without channel directory).
  FEED_BASE: 'https://client-download.plaud.ai/desktop',
  // Electron release the official app is built with (process.versions.electron
  // of Plaud 1.4.1); the Linux build uses the same stock release.
  ELECTRON_VERSION: '36.7.4',
  PROJECT_URL: 'https://github.com/trukhinyuri/plaud-linux-unofficial',
  // Contact for the packages: deb Maintainer, rpm Packager, pacman packager,
  // AppStream developer and update contact.
  MAINTAINER_NAME: 'Iurii Trukhin',
  MAINTAINER_EMAIL: 'yuri@trukhin.com',
});
