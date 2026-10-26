'use strict';

// Minimal, dependency-free ZIP reader (PKWARE APPNOTE 6.3.x subset):
// stored/deflated entries, ZIP64, Unix modes and symlinks, CRC-32 verified.
// Used to unpack the official macOS zip (app.asar, icon) and the Electron zip.

const path = require('node:path');
const zlib = require('node:zlib');
const { Transform } = require('node:stream');
const { pipeline } = require('node:stream/promises');
const fs = require('./fs');

const SIG_EOCD = 0x06054b50;
const SIG_ZIP64_LOCATOR = 0x07064b50;
const SIG_ZIP64_EOCD = 0x06064b50;
const SIG_CENTRAL = 0x02014b50;
const SIG_LOCAL = 0x04034b50;
const S_IFMT = 0o170000;
const S_IFLNK = 0o120000;
const S_IFDIR = 0o040000;

class ZipError extends Error {}

function readAt(fd, position, length) {
  const buf = Buffer.alloc(length);
  let done = 0;
  while (done < length) {
    const n = fs.readSync(fd, buf, done, length - done, position + done);
    if (n === 0) throw new ZipError(`unexpected end of file at ${position + done}`);
    done += n;
  }
  return buf;
}

function findEocd(fd, fileSize) {
  const maxBack = Math.min(fileSize, 0xffff + 22);
  const tail = readAt(fd, fileSize - maxBack, maxBack);
  for (let i = tail.length - 22; i >= 0; i--) {
    if (tail.readUInt32LE(i) === SIG_EOCD) {
      const commentLength = tail.readUInt16LE(i + 20);
      if (i + 22 + commentLength === tail.length) {
        return { offset: fileSize - maxBack + i, record: tail.subarray(i, i + 22) };
      }
    }
  }
  throw new ZipError('end of central directory record not found (not a zip file?)');
}

function readCentralDirectoryLocation(fd, fileSize) {
  const { offset: eocdOffset, record } = findEocd(fd, fileSize);
  let total = record.readUInt16LE(10);
  let cdSize = record.readUInt32LE(12);
  let cdOffset = record.readUInt32LE(16);
  if (total === 0xffff || cdSize === 0xffffffff || cdOffset === 0xffffffff) {
    if (eocdOffset < 20) throw new ZipError('ZIP64 locator missing');
    const locator = readAt(fd, eocdOffset - 20, 20);
    if (locator.readUInt32LE(0) !== SIG_ZIP64_LOCATOR) throw new ZipError('ZIP64 locator missing');
    const z64Offset = Number(locator.readBigUInt64LE(8));
    const z64 = readAt(fd, z64Offset, 56);
    if (z64.readUInt32LE(0) !== SIG_ZIP64_EOCD) throw new ZipError('ZIP64 end of central directory missing');
    total = Number(z64.readBigUInt64LE(32));
    cdSize = Number(z64.readBigUInt64LE(40));
    cdOffset = Number(z64.readBigUInt64LE(48));
  }
  return { total, cdSize, cdOffset };
}

function parseZip64Extra(extra, entry, rawDiskStart) {
  let pos = 0;
  while (pos + 4 <= extra.length) {
    const id = extra.readUInt16LE(pos);
    const size = extra.readUInt16LE(pos + 2);
    const body = extra.subarray(pos + 4, pos + 4 + size);
    if (id === 0x0001) {
      let p = 0;
      if (entry.size === 0xffffffff) { entry.size = Number(body.readBigUInt64LE(p)); p += 8; }
      if (entry.compressedSize === 0xffffffff) { entry.compressedSize = Number(body.readBigUInt64LE(p)); p += 8; }
      if (entry.localHeaderOffset === 0xffffffff) { entry.localHeaderOffset = Number(body.readBigUInt64LE(p)); p += 8; }
      if (rawDiskStart === 0xffff) p += 4;
    }
    pos += 4 + size;
  }
}

class ZipFile {
  constructor(file) {
    this.file = file;
    this.fd = fs.openSync(file, 'r');
    try {
      this.size = fs.fstatSync(this.fd).size;
      this.entries = this.#readEntries();
    } catch (error) {
      fs.closeSync(this.fd);
      throw error;
    }
    this.byName = new Map(this.entries.map((e) => [e.name, e]));
  }

  #readEntries() {
    const { total, cdSize, cdOffset } = readCentralDirectoryLocation(this.fd, this.size);
    const cd = readAt(this.fd, cdOffset, cdSize);
    const entries = [];
    let pos = 0;
    for (let i = 0; i < total; i++) {
      if (cd.readUInt32LE(pos) !== SIG_CENTRAL) throw new ZipError(`bad central directory entry #${i}`);
      const madeBy = cd.readUInt16LE(pos + 4);
      const flags = cd.readUInt16LE(pos + 8);
      const nameLength = cd.readUInt16LE(pos + 28);
      const extraLength = cd.readUInt16LE(pos + 30);
      const commentLength = cd.readUInt16LE(pos + 32);
      const rawDiskStart = cd.readUInt16LE(pos + 34);
      const externalAttrs = cd.readUInt32LE(pos + 38);
      const name = cd.toString('utf8', pos + 46, pos + 46 + nameLength);
      const entry = {
        name,
        flags,
        method: cd.readUInt16LE(pos + 10),
        crc32: cd.readUInt32LE(pos + 16),
        compressedSize: cd.readUInt32LE(pos + 20),
        size: cd.readUInt32LE(pos + 24),
        localHeaderOffset: cd.readUInt32LE(pos + 42),
        mode: 0,
        isDirectory: name.endsWith('/'),
        isSymlink: false,
      };
      parseZip64Extra(cd.subarray(pos + 46 + nameLength, pos + 46 + nameLength + extraLength), entry, rawDiskStart);
      if (madeBy >> 8 === 3) {
        // Unix "version made by": the high 16 bits of the external attributes
        // carry st_mode.
        entry.mode = externalAttrs >>> 16;
        const type = entry.mode & S_IFMT;
        entry.isSymlink = type === S_IFLNK;
        entry.isDirectory = entry.isDirectory || type === S_IFDIR;
      }
      if (name.includes('\0') || name.split('/').includes('..') || path.isAbsolute(name)) {
        throw new ZipError(`refusing unsafe entry name: ${JSON.stringify(name)}`);
      }
      entries.push(entry);
      pos += 46 + nameLength + extraLength + commentLength;
    }
    return entries;
  }

  #dataOffset(entry) {
    const local = readAt(this.fd, entry.localHeaderOffset, 30);
    if (local.readUInt32LE(0) !== SIG_LOCAL) throw new ZipError(`bad local header for ${entry.name}`);
    if (entry.flags & 0x1) throw new ZipError(`encrypted entry not supported: ${entry.name}`);
    if (entry.method !== 0 && entry.method !== 8) {
      throw new ZipError(`unsupported compression method ${entry.method} for ${entry.name}`);
    }
    return entry.localHeaderOffset + 30 + local.readUInt16LE(26) + local.readUInt16LE(28);
  }

  get(name) {
    return this.byName.get(name);
  }

  /** Reads a whole entry into memory, verifying size and CRC-32. */
  read(entryOrName) {
    const entry = typeof entryOrName === 'string' ? this.get(entryOrName) : entryOrName;
    if (!entry) throw new ZipError(`entry not found: ${entryOrName}`);
    const raw = readAt(this.fd, this.#dataOffset(entry), entry.compressedSize);
    const data = entry.method === 8 ? zlib.inflateRawSync(raw) : raw;
    if (data.length !== entry.size) throw new ZipError(`size mismatch for ${entry.name}`);
    if (zlib.crc32(data) !== entry.crc32) throw new ZipError(`CRC-32 mismatch for ${entry.name}`);
    return data;
  }

  /** Streams an entry to `dest`, verifying size and CRC-32; applies the Unix mode. */
  async extract(entryOrName, dest) {
    const entry = typeof entryOrName === 'string' ? this.get(entryOrName) : entryOrName;
    if (!entry) throw new ZipError(`entry not found: ${entryOrName}`);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    if (entry.isDirectory) {
      fs.mkdirSync(dest, { recursive: true });
      return;
    }
    if (entry.isSymlink) {
      fs.rmSync(dest, { force: true });
      fs.symlinkSync(this.read(entry).toString('utf8'), dest);
      return;
    }
    const start = this.#dataOffset(entry);
    const tmp = `${dest}.part-${process.pid}`;
    let crc = 0;
    let size = 0;
    try {
      if (entry.compressedSize === 0) {
        // Stored empty file: nothing to stream.
        fs.writeFileSync(tmp, '');
      } else {
        const verify = new Transform({
          transform(chunk, _enc, cb) {
            crc = zlib.crc32(chunk, crc);
            size += chunk.length;
            cb(null, chunk);
          },
        });
        const source = fs.createReadStream(null, {
          fd: this.fd,
          autoClose: false,
          start,
          end: start + entry.compressedSize - 1,
        });
        const stages = [source];
        if (entry.method === 8) stages.push(zlib.createInflateRaw());
        stages.push(verify, fs.createWriteStream(tmp, { mode: 0o644 }));
        await pipeline(stages);
      }
      if (size !== entry.size) throw new ZipError(`size mismatch for ${entry.name}`);
      if (crc !== entry.crc32) throw new ZipError(`CRC-32 mismatch for ${entry.name}`);
    } catch (error) {
      fs.rmSync(tmp, { force: true });
      throw error;
    }
    const perm = entry.mode & 0o777;
    fs.chmodSync(tmp, perm !== 0 ? perm : 0o644);
    fs.renameSync(tmp, dest);
  }

  /**
   * Extracts every entry whose name starts with `prefix` into `destDir`,
   * stripping the prefix. Returns the number of entries written.
   */
  async extractTree(prefix, destDir, { filter } = {}) {
    let count = 0;
    const symlinks = [];
    for (const entry of this.entries) {
      if (!entry.name.startsWith(prefix) || entry.name === prefix) continue;
      const rel = entry.name.slice(prefix.length);
      if (filter && !filter(rel, entry)) continue;
      // Never write through a symlink extracted earlier (it may point anywhere).
      if (symlinks.some((link) => rel.startsWith(`${link}/`))) {
        throw new ZipError(`refusing entry below a symlink: ${JSON.stringify(entry.name)}`);
      }
      await this.extract(entry, path.join(destDir, rel));
      if (entry.isSymlink) symlinks.push(rel.replace(/\/+$/, ''));
      count++;
    }
    return count;
  }

  close() {
    if (this.fd !== null) {
      fs.closeSync(this.fd);
      this.fd = null;
    }
  }
}

function openZip(file) {
  return new ZipFile(file);
}

module.exports = { openZip, ZipFile, ZipError };
