import {test} from 'node:test';
import assert from 'node:assert/strict';
import {HttpTransport} from '../web/http.mjs';

test('HTTP uses the same paths/parameters, encodes values and reports device errors', async () => {
  const original = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, options) => {
    calls.push({url, ...options});
    return new Response(url.endsWith('/set') ? 'Invalid motor settings.' : '[]', {status: url.endsWith('/set') ? 400 : 200});
  };
  try {
    const client = new HttpTransport();
    await client.open('192.168.1.50');
    assert.deepEqual(await client.request('/state'), []);
    await client.request('/mode', {ssid: 'A & + ü'});
    assert.equal(calls[0].method, 'GET');
    assert.equal(calls[1].method, 'POST');
    assert.equal(calls[1].body.get('ssid'), 'A & + ü');
    await assert.rejects(client.request('/set', {id: 3}), /Invalid motor settings/);
    assert.equal(calls.length, 3);
    await client.close();
  } finally { globalThis.fetch = original; }
});

test('Stop cancels queued Apply and HTTP commands remain serialized', async () => {
  const original = globalThis.fetch;
  const calls = [];
  let release;
  globalThis.fetch = (url) => {
    calls.push(url);
    return calls.length === 1 ? new Promise(resolve => { release = () => resolve(new Response('[]')); }) : Promise.resolve(new Response('[]'));
  };
  try {
    const client = new HttpTransport();
    await client.open('http://192.168.1.50');
    const read = client.request('/state');
    const apply = assert.rejects(client.request('/set', {id: 0}), /cancelled by Stop/);
    const stop = client.request('/stop', {id: 'all'});
    assert.equal(calls.length, 1);
    release();
    await Promise.all([read, apply, stop]);
    assert.deepEqual(calls.map(url => new URL(url).pathname), ['/state', '/stop']);
    await client.close();
  } finally { globalThis.fetch = original; }
});

test('Timeout aborts without replaying mutations', async () => {
  const original = globalThis.fetch;
  let calls = 0;
  globalThis.fetch = (url, options) => new Promise((resolve, reject) => {
    calls++;
    options.signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
  });
  try {
    const client = new HttpTransport();
    await client.open('http://192.168.1.50');
    await assert.rejects(client.request('/set', {id: 0}, 10), /not confirmed/);
    assert.equal(calls, 1);
    await client.close();
  } finally { globalThis.fetch = original; }
});
