'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const dbus = require('../linux-runtime/dbus');

function roundTrip(signature, body) {
  const buf = dbus.encodeMessage({ type: 1, serial: 7, path: '/a/b', interface: 'x.y.Z', member: 'M', destination: 'x.y', signature, body });
  assert.equal(buf.length % 1, 0);
  assert.equal(dbus.messageLength(buf), buf.length);
  assert.equal(dbus.messageLength(buf.subarray(0, buf.length - 1)), 0, 'incomplete messages are not consumed');
  return dbus.decodeMessage(buf);
}

test('signatures parse into type trees and back', () => {
  for (const sig of ['s', 'a{sv}', 'oa(ssv)', 'aa{sa(ii)}', '(yb(nq)xtd)', 'ah', 'ag']) {
    assert.equal(dbus.parseSignature(sig).map(dbus.typeToString).join(''), sig);
  }
  assert.throws(() => dbus.parseSignature('(ss'), /InvalidSignature|\(ss/);
  assert.throws(() => dbus.parseSignature('z'));
});

test('basic types survive encoding with correct alignment', () => {
  const msg = roundTrip('ybnqiuxtdsog', [255, true, -2, 65535, -70000, 4000000000, -(2n ** 40n), 2n ** 63n, 0.5, 'héllo', '/o/p', 'a{sv}']);
  assert.deepEqual(msg.body, [255, true, -2, 65535, -70000, 4000000000, -(2n ** 40n), 2n ** 63n, 0.5, 'héllo', '/o/p', 'a{sv}']);
  assert.equal(msg.member, 'M');
  assert.equal(msg.destination, 'x.y');
  assert.equal(msg.serial, 7);
});

test('containers: arrays, structs, dictionaries and variants', () => {
  const options = { handle_token: ['s', 'tok'], modal: ['b', true], count: ['u', 3], nested: ['a{sv}', { inner: ['as', ['a', 'b']] }] };
  const shortcuts = [
    ['highlight', { description: ['s', 'Highlight'], preferred_trigger: ['s', 'ALT+SHIFT+h'] }],
    ['screenshot', { description: ['s', 'Screenshot'] }],
  ];
  const msg = roundTrip('a{sv}a(sa{sv})ayax(ad)', [options, shortcuts, [1, 2, 3], [], [[1.5, 2.5]]]);
  assert.deepEqual(msg.body[0], { handle_token: 'tok', modal: true, count: 3, nested: { inner: ['a', 'b'] } });
  assert.deepEqual(msg.body[1], [
    ['highlight', { description: 'Highlight', preferred_trigger: 'ALT+SHIFT+h' }],
    ['screenshot', { description: 'Screenshot' }],
  ]);
  assert.deepEqual(msg.body[2], [1, 2, 3]);
  assert.deepEqual(msg.body[3], [], 'empty arrays keep element alignment');
  assert.deepEqual(msg.body[4], [[1.5, 2.5]]);
  // Maps work as dictionary input as well.
  assert.deepEqual(roundTrip('a{sv}', [new Map([['k', ['i', -1]]])]).body[0], { k: -1 });
  assert.throws(() => roundTrip('ss', ['only one']), /needs 2 values/);
});

test('bus addresses', () => {
  assert.equal(dbus.parseAddress('unix:path=/run/user/1000/bus'), '/run/user/1000/bus');
  assert.equal(dbus.parseAddress('tcp:host=x;unix:abstract=/tmp/dbus-abc,guid=1'), '\0/tmp/dbus-abc');
  assert.equal(dbus.parseAddress('unix:path=/tmp/a%20b'), '/tmp/a b');
  assert.throws(() => dbus.parseAddress('tcp:host=localhost,port=1'), /unsupported bus address/);
});

const live = process.env.DBUS_SESSION_BUS_ADDRESS ? false : 'no session bus (DBUS_SESSION_BUS_ADDRESS is unset)';

test('talks to the real session bus: calls, errors, properties and signals', { skip: live }, async (t) => {
  const conn = await dbus.DBusConnection.connect();
  t.after(() => conn.close());
  assert.match(conn.uniqueName, /^:\d+\.\d+$/);
  const BUS = { destination: 'org.freedesktop.DBus', path: '/org/freedesktop/DBus' };

  const [names] = await conn.call({ ...BUS, interface: 'org.freedesktop.DBus', member: 'ListNames' });
  assert.ok(names.includes('org.freedesktop.DBus'));
  assert.ok(names.includes(conn.uniqueName));
  const [pid] = await conn.call({ ...BUS, interface: 'org.freedesktop.DBus', member: 'GetConnectionUnixProcessID', signature: 's', body: [conn.uniqueName] });
  assert.equal(pid, process.pid);
  const [xml] = await conn.call({ ...BUS, interface: 'org.freedesktop.DBus.Introspectable', member: 'Introspect' });
  assert.match(xml, /<interface name="org\.freedesktop\.DBus">/);

  await assert.rejects(
    conn.call({ ...BUS, interface: 'org.freedesktop.DBus', member: 'GetNameOwner', signature: 's', body: ['org.example.Nobody.Here'] }),
    (error) => error instanceof dbus.DBusError && error.dbusName === 'org.freedesktop.DBus.Error.NameHasNoOwner',
  );

  // A second connection takes a well-known name; the first one sees the
  // NameOwnerChanged signal through its match rule.
  const name = `io.github.trukhinyuri.PlaudLinux.Test${process.pid}`;
  const seen = new Promise((resolve) => {
    conn
      .subscribe({ interface: 'org.freedesktop.DBus', member: 'NameOwnerChanged', arg0: name }, (msg) => resolve(msg.body))
      .then((unsubscribe) => t.after(unsubscribe));
  });
  const other = await dbus.DBusConnection.connect();
  t.after(() => other.close());
  await new Promise((r) => setTimeout(r, 100));
  const [reply] = await other.call({ ...BUS, interface: 'org.freedesktop.DBus', member: 'RequestName', signature: 'su', body: [name, 4] });
  assert.equal(reply, 1, 'primary owner');
  const timeout = new Promise((_, reject) => setTimeout(() => reject(new Error('no signal within 5 s')), 5000).unref());
  const [changedName, oldOwner, newOwner] = await Promise.race([seen, timeout]);
  assert.deepEqual([changedName, oldOwner, newOwner], [name, '', other.uniqueName]);
});
