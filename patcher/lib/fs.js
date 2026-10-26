'use strict';

// Inside Electron, the regular `fs` module treats *.asar files as directories.
// The patcher always needs raw file access, so it uses `original-fs` when it
// runs under Electron (in-app updater) and plain `fs` under Node (build).

module.exports = process.versions.electron ? require('original-fs') : require('node:fs');
