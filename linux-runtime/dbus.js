'use strict';

// Minimal D-Bus client (session bus, little-endian, EXTERNAL auth) for the
// XDG desktop portal calls the runtime makes. Implements the wire format of
// the D-Bus specification for the basic types, arrays, structs, dict entries
// and variants; no Unix fd passing.
//
// Values: strings/object paths/signatures are JS strings, 64-bit integers
// unmarshal to BigInt, arrays to arrays, dict arrays a{..} to plain objects
// when the key is a string (otherwise arrays of [key, value]), structs to
// arrays and variants to their value. Variants are marshalled from
// [signature, value] pairs.

const EventEmitter = require('node:events');
const net = require('node:net');

class DBusError extends Error {
  constructor(name, message) {
    super(message ? `${name}: ${message}` : name);
    this.name = 'DBusError';
    this.dbusName = name;
  }
}

const ALIGN = { y: 1, b: 4, n: 2, q: 2, i: 4, u: 4, x: 8, t: 8, d: 8, s: 4, o: 4, g: 1, a: 4, '(': 8, '{': 8, v: 1, h: 4 };

/** Parses a signature into a list of type nodes. */
function parseSignature(sig) {
  let pos = 0;
  const one = () => {
    const c = sig[pos++];
    if (c === 'a') return { code: 'a', child: one() };
    if (c === '(' || c === '{') {
      const close = c === '(' ? ')' : '}';
      const children = [];
      while (sig[pos] !== close) {
        if (pos >= sig.length) throw new DBusError('org.freedesktop.DBus.Error.InvalidSignature', sig);
        children.push(one());
      }
      pos++;
      return { code: c, children };
    }
    if (!(c in ALIGN)) throw new DBusError('org.freedesktop.DBus.Error.InvalidSignature', sig);
    return { code: c };
  };
  const out = [];
  while (pos < sig.length) out.push(one());
  return out;
}

function typeToString(t) {
  if (t.code === 'a') return `a${typeToString(t.child)}`;
  if (t.code === '(') return `(${t.children.map(typeToString).join('')})`;
  if (t.code === '{') return `{${t.children.map(typeToString).join('')}}`;
  return t.code;
}

class Writer {
  constructor() {
    this.chunks = [];
    this.length = 0;
  }

  bytes(buf) {
    this.chunks.push(buf);
    this.length += buf.length;
  }

  align(n) {
    const pad = (n - (this.length % n)) % n;
    if (pad) this.bytes(Buffer.alloc(pad));
  }

  write(t, value) {
    this.align(ALIGN[t.code]);
    let b;
    switch (t.code) {
      case 'y':
        b = Buffer.alloc(1);
        b.writeUInt8(value);
        return this.bytes(b);
      case 'b':
        b = Buffer.alloc(4);
        b.writeUInt32LE(value ? 1 : 0);
        return this.bytes(b);
      case 'n':
        b = Buffer.alloc(2);
        b.writeInt16LE(value);
        return this.bytes(b);
      case 'q':
        b = Buffer.alloc(2);
        b.writeUInt16LE(value);
        return this.bytes(b);
      case 'i':
        b = Buffer.alloc(4);
        b.writeInt32LE(value);
        return this.bytes(b);
      case 'u':
      case 'h':
        b = Buffer.alloc(4);
        b.writeUInt32LE(value);
        return this.bytes(b);
      case 'x':
        b = Buffer.alloc(8);
        b.writeBigInt64LE(BigInt(value));
        return this.bytes(b);
      case 't':
        b = Buffer.alloc(8);
        b.writeBigUInt64LE(BigInt(value));
        return this.bytes(b);
      case 'd':
        b = Buffer.alloc(8);
        b.writeDoubleLE(value);
        return this.bytes(b);
      case 's':
      case 'o': {
        const s = Buffer.from(String(value), 'utf8');
        b = Buffer.alloc(4);
        b.writeUInt32LE(s.length);
        this.bytes(b);
        this.bytes(s);
        return this.bytes(Buffer.alloc(1));
      }
      case 'g': {
        const s = Buffer.from(String(value), 'ascii');
        this.bytes(Buffer.from([s.length]));
        this.bytes(s);
        return this.bytes(Buffer.alloc(1));
      }
      case 'v': {
        const [sig, inner] = value;
        const types = parseSignature(sig);
        if (types.length !== 1) throw new DBusError('org.freedesktop.DBus.Error.InvalidArgs', `variant needs one type: ${sig}`);
        this.write({ code: 'g' }, sig);
        return this.write(types[0], inner);
      }
      case '(':
      case '{':
        t.children.forEach((child, i) => this.write(child, value[i]));
        return undefined;
      case 'a': {
        const lenPos = this.chunks.length;
        this.bytes(Buffer.alloc(4));
        this.align(ALIGN[t.child.code]);
        const start = this.length;
        let items = value;
        if (t.child.code === '{' && !Array.isArray(value)) {
          items = value instanceof Map ? [...value.entries()] : Object.entries(value);
        }
        for (const item of items) this.write(t.child, item);
        this.chunks[lenPos].writeUInt32LE(this.length - start);
        return undefined;
      }
      default:
        throw new DBusError('org.freedesktop.DBus.Error.InvalidSignature', t.code);
    }
  }

  toBuffer() {
    return Buffer.concat(this.chunks, this.length);
  }
}

class Reader {
  constructor(buf, offset = 0) {
    this.buf = buf;
    this.pos = offset;
  }

  align(n) {
    this.pos += (n - (this.pos % n)) % n;
  }

  read(t) {
    this.align(ALIGN[t.code]);
    const b = this.buf;
    let v;
    switch (t.code) {
      case 'y':
        v = b.readUInt8(this.pos);
        this.pos += 1;
        return v;
      case 'b':
        v = b.readUInt32LE(this.pos) !== 0;
        this.pos += 4;
        return v;
      case 'n':
        v = b.readInt16LE(this.pos);
        this.pos += 2;
        return v;
      case 'q':
        v = b.readUInt16LE(this.pos);
        this.pos += 2;
        return v;
      case 'i':
        v = b.readInt32LE(this.pos);
        this.pos += 4;
        return v;
      case 'u':
      case 'h':
        v = b.readUInt32LE(this.pos);
        this.pos += 4;
        return v;
      case 'x':
        v = b.readBigInt64LE(this.pos);
        this.pos += 8;
        return v;
      case 't':
        v = b.readBigUInt64LE(this.pos);
        this.pos += 8;
        return v;
      case 'd':
        v = b.readDoubleLE(this.pos);
        this.pos += 8;
        return v;
      case 's':
      case 'o': {
        const len = b.readUInt32LE(this.pos);
        this.pos += 4;
        v = b.toString('utf8', this.pos, this.pos + len);
        this.pos += len + 1;
        return v;
      }
      case 'g': {
        const len = b.readUInt8(this.pos);
        this.pos += 1;
        v = b.toString('ascii', this.pos, this.pos + len);
        this.pos += len + 1;
        return v;
      }
      case 'v': {
        const sig = this.read({ code: 'g' });
        const types = parseSignature(sig);
        return this.read(types[0]);
      }
      case '(':
      case '{':
        return t.children.map((child) => this.read(child));
      case 'a': {
        const len = b.readUInt32LE(this.pos);
        this.pos += 4;
        this.align(ALIGN[t.child.code]);
        const end = this.pos + len;
        const items = [];
        while (this.pos < end) items.push(this.read(t.child));
        if (t.child.code === '{' && t.child.children[0].code === 's') return Object.fromEntries(items);
        return items;
      }
      default:
        throw new DBusError('org.freedesktop.DBus.Error.InvalidSignature', t.code);
    }
  }
}

const TYPE = { METHOD_CALL: 1, METHOD_RETURN: 2, ERROR: 3, SIGNAL: 4 };
const FIELD = { PATH: 1, INTERFACE: 2, MEMBER: 3, ERROR_NAME: 4, REPLY_SERIAL: 5, DESTINATION: 6, SENDER: 7, SIGNATURE: 8 };
const FIELD_SIG = { 1: 'o', 2: 's', 3: 's', 4: 's', 5: 'u', 6: 's', 7: 's', 8: 'g' };
const HEADER_TYPE = parseSignature('yyyyuua(yv)');

/** Serialises one message. */
function encodeMessage({ type, serial, flags = 0, path, interface: iface, member, destination, signature = '', body = [] }) {
  const bodyWriter = new Writer();
  const types = parseSignature(signature);
  if (types.length !== body.length) throw new DBusError('org.freedesktop.DBus.Error.InvalidArgs', `signature ${signature} needs ${types.length} values`);
  types.forEach((t, i) => bodyWriter.write(t, body[i]));
  const bodyBuf = bodyWriter.toBuffer();
  const fields = [];
  if (path) fields.push([FIELD.PATH, ['o', path]]);
  if (iface) fields.push([FIELD.INTERFACE, ['s', iface]]);
  if (member) fields.push([FIELD.MEMBER, ['s', member]]);
  if (destination) fields.push([FIELD.DESTINATION, ['s', destination]]);
  if (signature) fields.push([FIELD.SIGNATURE, ['g', signature]]);
  const header = new Writer();
  const values = ['l'.charCodeAt(0), type, flags, 1, bodyBuf.length, serial, fields];
  HEADER_TYPE.forEach((t, i) => header.write(t, values[i]));
  header.align(8);
  return Buffer.concat([header.toBuffer(), bodyBuf]);
}

/** Returns the total length of the message at the start of `buf`, or 0 if incomplete. */
function messageLength(buf) {
  if (buf.length < 16) return 0;
  if (buf[0] !== 0x6c) throw new DBusError('org.freedesktop.DBus.Error.NotSupported', 'big-endian messages are not supported');
  const bodyLength = buf.readUInt32LE(4);
  const fieldsLength = buf.readUInt32LE(12);
  const headerLength = 16 + fieldsLength + ((8 - ((16 + fieldsLength) % 8)) % 8);
  const total = headerLength + bodyLength;
  return buf.length >= total ? total : 0;
}

function decodeMessage(buf) {
  const reader = new Reader(buf);
  const [, type, flags, , bodyLength, serial, rawFields] = HEADER_TYPE.map((t) => reader.read(t));
  reader.align(8);
  const fields = {};
  for (const [code, value] of rawFields) fields[code] = value;
  const signature = fields[FIELD.SIGNATURE] ?? '';
  const bodyReader = new Reader(buf.subarray(reader.pos, reader.pos + bodyLength));
  const body = parseSignature(signature).map((t) => bodyReader.read(t));
  return {
    type,
    flags,
    serial,
    path: fields[FIELD.PATH],
    interface: fields[FIELD.INTERFACE],
    member: fields[FIELD.MEMBER],
    errorName: fields[FIELD.ERROR_NAME],
    replySerial: fields[FIELD.REPLY_SERIAL],
    destination: fields[FIELD.DESTINATION],
    sender: fields[FIELD.SENDER],
    signature,
    body,
  };
}

function parseAddress(address) {
  for (const part of String(address).split(';')) {
    const [transport, rest = ''] = part.split(/:(.*)/s);
    if (transport !== 'unix') continue;
    const params = Object.fromEntries(rest.split(',').map((kv) => kv.split('=').map(decodeURIComponent)));
    if (params.path) return params.path;
    if (params.abstract) return `\0${params.abstract}`;
  }
  throw new DBusError('org.freedesktop.DBus.Error.BadAddress', `unsupported bus address: ${address}`);
}

class DBusConnection extends EventEmitter {
  constructor(socket) {
    super();
    this.socket = socket;
    this.serial = 0;
    this.pending = new Map();
    this.buffer = Buffer.alloc(0);
    this.uniqueName = null;
    this.closed = false;
    socket.on('data', (d) => this.#onData(d));
    socket.on('close', () => this.#onClose(new DBusError('org.freedesktop.DBus.Error.Disconnected', 'connection closed')));
    socket.on('error', (e) => this.#onClose(e));
  }

  /** Connects and authenticates to the session bus (or `address`). */
  static connect(address = process.env.DBUS_SESSION_BUS_ADDRESS, { timeoutMs = 5000 } = {}) {
    return new Promise((resolve, reject) => {
      let path;
      try {
        if (!address) throw new DBusError('org.freedesktop.DBus.Error.BadAddress', 'DBUS_SESSION_BUS_ADDRESS is not set');
        path = parseAddress(address);
      } catch (error) {
        reject(error);
        return;
      }
      const socket = net.createConnection(path);
      const timer = setTimeout(() => {
        socket.destroy();
        reject(new DBusError('org.freedesktop.DBus.Error.Timeout', 'connecting to the session bus timed out'));
      }, timeoutMs);
      let authBuf = '';
      const onAuthData = (data) => {
        authBuf += data.toString('ascii');
        if (!authBuf.includes('\r\n')) return;
        socket.off('data', onAuthData);
        if (!authBuf.startsWith('OK ')) {
          clearTimeout(timer);
          socket.destroy();
          reject(new DBusError('org.freedesktop.DBus.Error.AuthFailed', authBuf.trim()));
          return;
        }
        socket.write('BEGIN\r\n');
        const conn = new DBusConnection(socket);
        conn
          .call({ destination: 'org.freedesktop.DBus', path: '/org/freedesktop/DBus', interface: 'org.freedesktop.DBus', member: 'Hello' })
          .then(([name]) => {
            clearTimeout(timer);
            conn.uniqueName = name;
            resolve(conn);
          }, (error) => {
            clearTimeout(timer);
            conn.close();
            reject(error);
          });
      };
      socket.once('connect', () => {
        const uid = Buffer.from(String(process.getuid()), 'ascii').toString('hex');
        socket.on('data', onAuthData);
        socket.write(Buffer.from([0]));
        socket.write(`AUTH EXTERNAL ${uid}\r\n`);
      });
      socket.once('error', (error) => {
        clearTimeout(timer);
        reject(error);
      });
    });
  }

  #onData(data) {
    this.buffer = this.buffer.length ? Buffer.concat([this.buffer, data]) : data;
    for (;;) {
      let len;
      try {
        len = messageLength(this.buffer);
      } catch (error) {
        this.#onClose(error);
        return;
      }
      if (!len) return;
      const raw = this.buffer.subarray(0, len);
      this.buffer = this.buffer.subarray(len);
      let msg;
      try {
        msg = decodeMessage(raw);
      } catch (error) {
        this.emit('protocol-error', error);
        continue;
      }
      this.#dispatch(msg);
    }
  }

  #dispatch(msg) {
    if (msg.type === TYPE.METHOD_RETURN || msg.type === TYPE.ERROR) {
      const waiter = this.pending.get(msg.replySerial);
      if (!waiter) return;
      this.pending.delete(msg.replySerial);
      if (msg.type === TYPE.ERROR) waiter.reject(new DBusError(msg.errorName, typeof msg.body[0] === 'string' ? msg.body[0] : ''));
      else waiter.resolve(msg.body);
    } else if (msg.type === TYPE.SIGNAL) {
      this.emit('signal', msg);
    }
  }

  #onClose(error) {
    if (this.closed) return;
    this.closed = true;
    for (const waiter of this.pending.values()) waiter.reject(error);
    this.pending.clear();
    this.emit('close', error);
  }

  /** Calls a method; resolves with the reply body (array). */
  call({ destination, path, interface: iface, member, signature = '', body = [], timeoutMs = 30_000 }) {
    if (this.closed) return Promise.reject(new DBusError('org.freedesktop.DBus.Error.Disconnected', 'connection closed'));
    const serial = ++this.serial;
    const msg = encodeMessage({ type: TYPE.METHOD_CALL, serial, path, interface: iface, member, destination, signature, body });
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(serial);
        reject(new DBusError('org.freedesktop.DBus.Error.NoReply', `${iface}.${member} timed out`));
      }, timeoutMs);
      timer.unref?.();
      this.pending.set(serial, {
        resolve: (v) => {
          clearTimeout(timer);
          resolve(v);
        },
        reject: (e) => {
          clearTimeout(timer);
          reject(e);
        },
      });
      this.socket.write(msg);
    });
  }

  /** Subscribes to signals matching `rule` (an object of match keys). Returns an unsubscribe function. */
  async subscribe(rule, handler) {
    const ruleString = Object.entries(rule).map(([k, v]) => `${k}='${v}'`).join(',');
    await this.call({
      destination: 'org.freedesktop.DBus',
      path: '/org/freedesktop/DBus',
      interface: 'org.freedesktop.DBus',
      member: 'AddMatch',
      signature: 's',
      body: [`type='signal',${ruleString}`],
    });
    const listener = (msg) => {
      if (rule.interface && msg.interface !== rule.interface) return;
      if (rule.member && msg.member !== rule.member) return;
      if (rule.path && msg.path !== rule.path) return;
      handler(msg);
    };
    this.on('signal', listener);
    return async () => {
      this.off('signal', listener);
      if (this.closed) return;
      await this.call({
        destination: 'org.freedesktop.DBus',
        path: '/org/freedesktop/DBus',
        interface: 'org.freedesktop.DBus',
        member: 'RemoveMatch',
        signature: 's',
        body: [`type='signal',${ruleString}`],
      }).catch(() => {});
    };
  }

  close() {
    if (!this.closed) this.socket.end();
    this.#onClose(new DBusError('org.freedesktop.DBus.Error.Disconnected', 'connection closed'));
  }
}

module.exports = { DBusConnection, DBusError, parseSignature, typeToString, encodeMessage, decodeMessage, messageLength, parseAddress, Writer, Reader };
