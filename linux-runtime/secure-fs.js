'use strict';

// Private files and directories for everything that decides which code the
// app runs: staged releases, their downloads and the boot bookkeeping.
//
// - Directories are created with mode 0700, set explicitly so that the umask
//   does not matter. Files are written through exclusive temporary files
//   (O_CREAT | O_EXCL | O_NOFOLLOW, mode 0600) renamed into place.
// - Before anything is written to or loaded from them, our directories are
//   checked with lstat(2): a real directory (not a symbolic link), owned by
//   the current user, not writable by its group or other users. Files are
//   opened without following symbolic links and checked the same way with
//   fstat(2). A failed check throws UnsafePathError; callers refuse the
//   operation and fall back to the packaged release.

const crypto = require('node:crypto');
const path = require('node:path');
// Inside Electron, `fs` treats *.asar files as archives; staged archives are
// checked and hashed as the plain files they are.
const fs = process.versions.electron ? require('original-fs') : require('node:fs');

const { O_RDONLY, O_WRONLY, O_CREAT, O_EXCL, O_NOFOLLOW, O_NONBLOCK, O_DIRECTORY } = fs.constants;

class UnsafePathError extends Error {
  constructor(file, reason) {
    super(`${file} ${reason}`);
    this.name = 'UnsafePathError';
    this.code = 'ERR_UNSAFE_PATH';
    this.path = file;
  }
}

function check(file, stats, kind) {
  const uid = process.getuid();
  if (stats.isSymbolicLink()) throw new UnsafePathError(file, 'is a symbolic link');
  if (kind === 'directory' ? !stats.isDirectory() : !stats.isFile()) throw new UnsafePathError(file, `is not a ${kind}`);
  if (stats.uid !== uid) throw new UnsafePathError(file, `is owned by uid ${stats.uid}, not by uid ${uid}`);
  if (stats.mode & 0o022) {
    const who = stats.mode & 0o020 ? 'its group' : 'other users';
    throw new UnsafePathError(file, `is writable by ${who} (mode ${(stats.mode & 0o7777).toString(8).padStart(4, '0')})`);
  }
  return stats;
}

/** Throws unless `dir` is a directory (not a symbolic link) of the current user that only it can write. */
function assertPrivateDir(dir) {
  return check(dir, fs.lstatSync(dir), 'directory');
}

/** Creates `dir` with mode 0700 whatever the umask. Returns false when it already existed. */
function makePrivateDir(dir) {
  try {
    fs.mkdirSync(dir, { mode: 0o700 });
  } catch (error) {
    if (error.code === 'EEXIST') return false;
    throw error;
  }
  const fd = fs.openSync(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  try {
    fs.fchmodSync(fd, 0o700);
  } finally {
    fs.closeSync(fd);
  }
  return true;
}

/**
 * Makes `dir`, which lies below `base`, a usable private directory and
 * returns it. Missing directories are created with mode 0700 (`base` and its
 * parents too, as the XDG Base Directory specification asks); every
 * directory below `base` down to `dir` is checked. `base` is part of the
 * user's environment and is not checked.
 */
function ensurePrivateDir(dir, base) {
  const rel = path.relative(base, dir);
  if (!rel || rel === '..' || rel.startsWith(`..${path.sep}`) || path.isAbsolute(rel)) throw new Error(`${dir} is not below ${base}`);
  fs.mkdirSync(base, { recursive: true, mode: 0o700 });
  let current = base;
  for (const part of rel.split(path.sep)) {
    current = path.join(current, part);
    makePrivateDir(current);
    assertPrivateDir(current);
  }
  return dir;
}

function openExclusive(file) {
  const fd = fs.openSync(file, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600);
  fs.fchmodSync(fd, 0o600);
  return fd;
}

const randomSuffix = () => crypto.randomBytes(8).toString('hex');

/** Writes `data` to `file` in a private directory: exclusive temporary file (mode 0600), then rename. */
function writePrivateFile(file, data) {
  const dir = path.dirname(file);
  assertPrivateDir(dir);
  const tmp = path.join(dir, `.${path.basename(file)}.${randomSuffix()}.tmp`);
  const fd = openExclusive(tmp);
  try {
    try {
      fs.writeFileSync(fd, data);
    } finally {
      fs.closeSync(fd);
    }
    fs.renameSync(tmp, file);
  } catch (error) {
    fs.rmSync(tmp, { force: true });
    throw error;
  }
}

/** Creates a new file (mode 0600) with a unique name starting with `prefix` in the private directory `dir`. */
function createPrivateFile(dir, prefix) {
  assertPrivateDir(dir);
  const file = path.join(dir, `${prefix}.${randomSuffix()}`);
  return { fd: openExclusive(file), file };
}

/**
 * Opens a private regular file for reading without following a symbolic
 * link (O_NONBLOCK: a FIFO must not block the open before it is refused);
 * null when it does not exist.
 */
function openPrivateFile(file) {
  let fd;
  try {
    fd = fs.openSync(file, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    if (error.code === 'ELOOP') throw new UnsafePathError(file, 'is a symbolic link');
    throw error;
  }
  try {
    check(file, fs.fstatSync(fd), 'file');
  } catch (error) {
    fs.closeSync(fd);
    throw error;
  }
  return fd;
}

/** Contents of a private regular file, or null when it does not exist. */
function readPrivateFile(file) {
  const fd = openPrivateFile(file);
  if (fd === null) return null;
  try {
    return fs.readFileSync(fd);
  } finally {
    fs.closeSync(fd);
  }
}

/** SHA-256 (hex) of a private regular file. */
function sha256File(file) {
  const fd = openPrivateFile(file);
  if (fd === null) throw new UnsafePathError(file, 'does not exist');
  try {
    const hash = crypto.createHash('sha256');
    const buffer = Buffer.allocUnsafe(1 << 20);
    let n;
    while ((n = fs.readSync(fd, buffer, 0, buffer.length, null)) > 0) hash.update(buffer.subarray(0, n));
    return hash.digest('hex');
  } finally {
    fs.closeSync(fd);
  }
}

/**
 * SHA-256 of every file below the private directory `root`, keyed by the
 * path relative to it ('/'-separated). Every directory and file met is
 * checked; `exclude` lists top-level names to leave out.
 */
function hashTree(root, { exclude = [] } = {}) {
  assertPrivateDir(root);
  const out = {};
  const walk = (dir, rel) => {
    for (const name of fs.readdirSync(dir).sort()) {
      if (!rel && exclude.includes(name)) continue;
      const full = path.join(dir, name);
      const relPath = rel ? `${rel}/${name}` : name;
      const stats = fs.lstatSync(full);
      if (stats.isDirectory()) {
        check(full, stats, 'directory');
        walk(full, relPath);
      } else {
        // Symbolic links, devices, FIFOs and sockets are refused before opening.
        check(full, stats, 'file');
        out[relPath] = sha256File(full);
      }
    }
  };
  walk(root, '');
  return out;
}

module.exports = {
  fs,
  UnsafePathError,
  assertPrivateDir,
  makePrivateDir,
  ensurePrivateDir,
  writePrivateFile,
  createPrivateFile,
  readPrivateFile,
  sha256File,
  hashTree,
};
