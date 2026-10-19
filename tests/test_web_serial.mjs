import {test} from 'node:test';
import assert from 'node:assert/strict';
import {SerialTransport} from '../web/serial.mjs';

class Port {
  constructor(reply = true) { this.reply = reply; this.commands = []; }
  async open(options) {
    assert.equal(options.baudRate, 115200);
    this.readable = new ReadableStream({start: c => this.receiver = c});
    this.writable = new WritableStream({write: bytes => {
      const command = new TextDecoder().decode(bytes);
      this.commands.push(command);
      if (this.reply) this.respond(command);
    }});
  }
  async setSignals(signals) { assert.equal(signals.dataTerminalReady, false); }
  async close() { this.closed = true; }
  respond(command, code = 200, data = {ok: true}) {
    const id = Number(command.split(' ')[1]);
    const text = 'boot log\nMC malformed\n' +
      `MC ${JSON.stringify({id: id + 1, code: 200, data: 'stale'})}\n` +
      `MC ${JSON.stringify({id, code, data})}\r\n`;
    const bytes = new TextEncoder().encode(text);
    for (let i = 0; i < bytes.length; i += 7) this.receiver.enqueue(bytes.slice(i, i + 7));
  }
}
const tick = () => new Promise(r => setTimeout(r, 0));

test('fragmented serial replies, boot logs, stale IDs and UTF-8', async () => {
  const port = new Port(false), serial = new SerialTransport();
  await serial.open(port);
  try {
    const result = serial.request('/state');
    await tick();
    port.respond(port.commands[0], 200, {ssid: 'café +&= 秘密'});
    assert.deepEqual(await result, {ssid: 'café +&= 秘密'});
  } finally { await serial.close(); }
});

test('commands serialize and URL encode arguments', async () => {
  const port = new Port(), serial = new SerialTransport();
  await serial.open(port);
  try {
    const ssid = 'A +&= café', password = 'secret +&=%';
    await Promise.all([serial.request('/state'), serial.request('/mode', {ssid, password})]);
    assert.equal(port.commands.length, 2);
    const params = new URLSearchParams(port.commands[1].split('?')[1].trim());
    assert.equal(params.get('ssid'), ssid);
    assert.equal(params.get('password'), password);
  } finally { await serial.close(); }
});

test('firmware validation errors are returned without retry', async () => {
  const port = new Port(false), serial = new SerialTransport();
  await serial.open(port);
  try {
    const result = assert.rejects(serial.request('/set'), /Invalid settings/);
    await tick(); port.respond(port.commands[0], 400, 'Invalid settings');
    await result;
    assert.equal(port.commands.length, 1);
  } finally { await serial.close(); }
});

test('Stop cancels queued Apply before it can re-enable outputs', async () => {
  const port = new Port(false), serial = new SerialTransport();
  await serial.open(port);
  try {
    const state = serial.request('/state');
    const cancelled = assert.rejects(serial.request('/set', {id: '0', enabled: '1'}), /cancelled by Stop/);
    const stop = serial.request('/stop', {id: 'all'});
    await tick(); port.respond(port.commands[0]);
    await state; await cancelled; await tick();
    assert.equal(port.commands.length, 2);
    assert.match(port.commands[1], /\/stop\?id=all/);
    port.respond(port.commands[1]); await stop;
  } finally { await serial.close(); }
});

test('timeout is unconfirmed and mutations are never replayed', async () => {
  const port = new Port(false), serial = new SerialTransport();
  await serial.open(port);
  try {
    await assert.rejects(serial.request('/set', {id: '3'}, 10), /may have been applied/);
    assert.equal(port.commands.length, 1);
  } finally { await serial.close(); }
});

test('oversized requests rejected before sending; close releases port', async () => {
  const port = new Port(), serial = new SerialTransport();
  await serial.open(port);
  await assert.rejects(serial.request('/mode', {ssid: 'x'.repeat(2000)}), /1536/);
  assert.equal(port.commands.length, 0);
  await serial.close();
  assert.equal(port.closed, true);
  assert.equal(serial.connected, false);
});

test('disconnect rejects active/queued requests and reconnect can succeed', async () => {
  const port = new Port(false), serial = new SerialTransport();
  await serial.open(port);
  const active = assert.rejects(serial.request('/state'), /disconnected/);
  const queued = assert.rejects(serial.request('/state'), /disconnected/);
  await serial.close(); await active; await queued;
  await serial.open(new Port());
  assert.deepEqual(await serial.request('/state'), {ok: true});
  await serial.close();
});
