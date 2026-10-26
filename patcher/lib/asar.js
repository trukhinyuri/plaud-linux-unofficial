'use strict';

// Minimal, dependency-free reader/writer for Electron's asar archive format,
// compatible with @electron/asar (see test/asar.test.js, which round-trips
// against it). Layout: an 8-byte Pickle holding the header size, a Pickle
// holding the JSON header string, then the concatenated file contents.
// Entries flagged `unpacked` live in "<archive>.unpacked/<path>".

const path = require('node:path');
const crypto = require('node:crypto');
const fs = require('./fs');

const INTEGRITY_BLOCK_SIZE = 4 * 1024 * 1024;

class AsarError extends Error {}

function readHeader(fd) {
  const sizeBuf = Buffer.alloc(8);
  if (fs.readSync(fd, sizeBuf, 0, 8, 0) !== 8) throw new AsarError('truncated asar size header');
  if (sizeBuf.readUInt32LE(0) !== 4) throw new AsarError('not an asar archive (bad size pickle)');
  const headerSize = sizeBuf.readUInt32LE(4);
  const headerBuf = Buffer.alloc(headerSize);
  if (fs.readSync(fd, headerBuf, 0, headerSize, 8) !== headerSize) throw new AsarError('truncated asar header');
  const payloadSize = headerBuf.readUInt32LE(0);
  const strLength = headerBuf.readUInt32LE(4);
  if (payloadSize + 4 !== headerSize || strLength + 8 > headerSize) throw new AsarError('corrupt asar header pickle');
  const json = headerBuf.toString('utf8', 8, 8 + strLength);
  return { header: JSON.parse(json), dataOffset: 8 + headerSize };
}

function integrityOf(data) {
  const blocks = [];
  for (let i = 0; i < data.length; i += INTEGRITY_BLOCK_SIZE) {
    blocks.push(crypto.createHash('sha256').update(data.subarray(i, i + INTEGRITY_BLOCK_SIZE)).digest('hex'));
  }
  if (blocks.length === 0) blocks.push(crypto.createHash('sha256').update(Buffer.alloc(0)).digest('hex'));
  return {
    algorithm: 'SHA256',
    hash: crypto.createHash('sha256').update(data).digest('hex'),
    blockSize: INTEGRITY_BLOCK_SIZE,
    blocks,
  };
}

function splitPath(p) {
  const norm = p.replace(/\\/g, '/').replace(/^\/+/, '').replace(/\/+$/, '');
  if (norm === '') return [];
  const parts = norm.split('/');
  if (parts.some((s) => s === '' || s === '.' || s === '..')) throw new AsarError(`invalid archive path: ${p}`);
  return parts;
}

/**
 * Mutable in-memory view of an asar archive. Unchanged file contents stay on
 * disk and are copied when the archive is written.
 */
class AsarArchive {
  constructor() {
    this.root = { files: Object.create(null) };
    this.sources = [];
  }

  /** Opens an existing archive (file contents are read lazily). */
  static open(archivePath) {
    const fd = fs.openSync(archivePath, 'r');
    let parsed;
    try {
      parsed = readHeader(fd);
    } catch (error) {
      fs.closeSync(fd);
      throw error;
    }
    const archive = new AsarArchive();
    const source = { fd, archivePath, dataOffset: parsed.dataOffset };
    archive.sources.push(source);
    archive.root = { files: Object.create(null) };
    const walk = (srcDir, dstDir, rel, inheritedUnpacked) => {
      for (const [name, node] of Object.entries(srcDir.files)) {
        const relPath = rel ? `${rel}/${name}` : name;
        if (node.files) {
          const dir = { files: Object.create(null) };
          const unpacked = inheritedUnpacked || node.unpacked === true;
          dstDir.files[name] = dir;
          walk(node, dir, relPath, unpacked);
        } else if (node.link !== undefined) {
          dstDir.files[name] = { link: node.link };
        } else {
          const unpacked = inheritedUnpacked || node.unpacked === true;
          dstDir.files[name] = {
            size: node.size,
            executable: node.executable === true,
            unpacked,
            integrity: node.integrity,
            source: unpacked
              ? { kind: 'unpacked', file: path.join(`${archivePath}.unpacked`, ...relPath.split('/')) }
              : { kind: 'archive', source, offset: Number(node.offset), size: node.size },
          };
        }
      }
    };
    walk(parsed.header, archive.root, '', false);
    return archive;
  }

  #lookup(p, { create = false } = {}) {
    const parts = splitPath(p);
    let dir = this.root;
    for (let i = 0; i < parts.length - 1; i++) {
      let next = dir.files[parts[i]];
      if (!next) {
        if (!create) return { parent: null, name: parts.at(-1) };
        next = { files: Object.create(null) };
        dir.files[parts[i]] = next;
      } else if (!next.files) {
        throw new AsarError(`not a directory: ${parts.slice(0, i + 1).join('/')}`);
      }
      dir = next;
    }
    return { parent: dir, name: parts.at(-1) };
  }

  /** Returns the entry node for `p` or undefined. */
  get(p) {
    if (splitPath(p).length === 0) return this.root;
    const { parent, name } = this.#lookup(p);
    return parent ? parent.files[name] : undefined;
  }

  has(p) {
    return this.get(p) !== undefined;
  }

  isDirectory(p) {
    return Boolean(this.get(p)?.files);
  }

  /** Lists the direct children names of directory `p`. */
  readdir(p = '') {
    const node = this.get(p);
    if (!node?.files) throw new AsarError(`not a directory: ${p}`);
    return Object.keys(node.files);
  }

  /** Lists every file (not directory) path, depth-first in header order. */
  listFiles() {
    const out = [];
    const walk = (dir, rel) => {
      for (const [name, node] of Object.entries(dir.files)) {
        const relPath = rel ? `${rel}/${name}` : name;
        if (node.files) walk(node, relPath);
        else out.push(relPath);
      }
    };
    walk(this.root, '');
    return out;
  }

  /** Reads the contents of file `p`. */
  readFile(p) {
    const node = this.get(p);
    if (!node || node.files) throw new AsarError(`no such file in archive: ${p}`);
    if (node.link !== undefined) throw new AsarError(`is a symlink: ${p}`);
    return readNode(node);
  }

  /** Creates or replaces file `p`. */
  writeFile(p, data, { executable = false, unpacked = false } = {}) {
    const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
    const { parent, name } = this.#lookup(p, { create: true });
    const existing = parent.files[name];
    if (existing?.files) throw new AsarError(`is a directory: ${p}`);
    parent.files[name] = {
      size: buf.length,
      executable,
      unpacked,
      integrity: integrityOf(buf),
      source: { kind: 'buffer', data: buf },
    };
  }

  /** Removes file or directory `p` (recursively). Returns true if it existed. */
  remove(p) {
    const { parent, name } = this.#lookup(p);
    if (!parent || !(name in parent.files)) return false;
    delete parent.files[name];
    return true;
  }

  /** Copies a directory tree from the real filesystem into the archive at `destPrefix`. */
  addDirectory(srcDir, destPrefix, { filter } = {}) {
    for (const entry of fs.readdirSync(srcDir, { withFileTypes: true })) {
      const src = path.join(srcDir, entry.name);
      const dst = destPrefix ? `${destPrefix}/${entry.name}` : entry.name;
      if (filter && !filter(src, entry)) continue;
      if (entry.isDirectory()) {
        this.addDirectory(src, dst, { filter });
      } else if (entry.isFile()) {
        const mode = fs.statSync(src).mode;
        this.writeFile(dst, fs.readFileSync(src), { executable: (mode & 0o100) !== 0 });
      }
    }
  }

  /**
   * Serialises the archive to `outPath` (and `<outPath>.unpacked/` for
   * unpacked entries). Writes to a temporary name first.
   */
  write(outPath) {
    const header = { files: Object.create(null) };
    const order = [];
    let offset = 0;
    const build = (dir, dst, rel) => {
      for (const [name, node] of Object.entries(dir.files)) {
        const relPath = rel ? `${rel}/${name}` : name;
        if (node.files) {
          const sub = { files: Object.create(null) };
          dst.files[name] = sub;
          build(node, sub, relPath);
        } else if (node.link !== undefined) {
          dst.files[name] = { link: node.link };
        } else {
          const entry = { size: node.size };
          if (node.unpacked) {
            entry.unpacked = true;
          } else {
            entry.offset = String(offset);
            offset += node.size;
          }
          if (node.executable) entry.executable = true;
          entry.integrity = node.integrity ?? integrityOf(readNode(node));
          dst.files[name] = entry;
          order.push({ relPath, node });
        }
      }
    };
    build(this.root, header, '');

    const json = Buffer.from(JSON.stringify(header), 'utf8');
    const padding = (4 - (json.length % 4)) % 4;
    const headerPickle = Buffer.alloc(8 + json.length + padding);
    headerPickle.writeUInt32LE(4 + json.length + padding, 0);
    headerPickle.writeUInt32LE(json.length, 4);
    json.copy(headerPickle, 8);
    const sizePickle = Buffer.alloc(8);
    sizePickle.writeUInt32LE(4, 0);
    sizePickle.writeUInt32LE(headerPickle.length, 4);

    const tmp = `${outPath}.tmp-${process.pid}`;
    const unpackedDir = `${outPath}.unpacked`;
    const tmpUnpacked = `${unpackedDir}.tmp-${process.pid}`;
    fs.rmSync(tmpUnpacked, { recursive: true, force: true });
    fs.mkdirSync(path.dirname(outPath), { recursive: true });
    const fd = fs.openSync(tmp, 'w', 0o644);
    let hasUnpacked = false;
    try {
      fs.writeSync(fd, sizePickle);
      fs.writeSync(fd, headerPickle);
      for (const { relPath, node } of order) {
        if (node.unpacked) {
          hasUnpacked = true;
          const dest = path.join(tmpUnpacked, ...relPath.split('/'));
          // Explicit modes: a staged release must not be writable by others whatever the umask.
          fs.mkdirSync(path.dirname(dest), { recursive: true, mode: 0o755 });
          fs.writeFileSync(dest, readNode(node), { mode: node.executable ? 0o755 : 0o644 });
        } else {
          copyNodeTo(node, fd);
        }
      }
      fs.fsyncSync(fd);
    } finally {
      fs.closeSync(fd);
    }
    fs.rmSync(unpackedDir, { recursive: true, force: true });
    if (hasUnpacked) fs.renameSync(tmpUnpacked, unpackedDir);
    fs.renameSync(tmp, outPath);
  }

  close() {
    for (const source of this.sources) {
      if (source.fd !== null) {
        fs.closeSync(source.fd);
        source.fd = null;
      }
    }
  }
}

function readNode(node) {
  const src = node.source;
  if (src.kind === 'buffer') return src.data;
  if (src.kind === 'unpacked') return fs.readFileSync(src.file);
  const buf = Buffer.alloc(src.size);
  let done = 0;
  while (done < src.size) {
    const n = fs.readSync(src.source.fd, buf, done, src.size - done, src.source.dataOffset + src.offset + done);
    if (n === 0) throw new AsarError(`truncated archive while reading ${src.source.archivePath}`);
    done += n;
  }
  return buf;
}

function copyNodeTo(node, fd) {
  const src = node.source;
  if (src.kind !== 'archive') {
    fs.writeSync(fd, readNode(node));
    return;
  }
  const chunk = Buffer.alloc(Math.min(src.size, 1 << 20));
  let done = 0;
  while (done < src.size) {
    const want = Math.min(chunk.length, src.size - done);
    const n = fs.readSync(src.source.fd, chunk, 0, want, src.source.dataOffset + src.offset + done);
    if (n === 0) throw new AsarError(`truncated archive while copying from ${src.source.archivePath}`);
    fs.writeSync(fd, chunk, 0, n);
    done += n;
  }
}

module.exports = { AsarArchive, AsarError, integrityOf, readHeader };
