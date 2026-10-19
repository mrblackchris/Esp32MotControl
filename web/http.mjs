// HTTP over TCP/IP. Same request(path, params) contract as SerialTransport.
export class HttpTransport extends EventTarget {
  constructor() {
    super();
    this.address = null;
    this.queue = [];
    this.active = null;
  }
  get connected() { return this.address !== null; }
  async open(address) {
    const url = new URL(address.includes('://') ? address : 'http://' + address);
    if (url.protocol !== 'http:' || url.username || url.password || url.pathname !== '/' || url.search || url.hash)
      throw new Error('Enter an HTTP device address, for example http://192.168.1.50');
    this.address = url.origin;
  }
  request(path, params = {}, timeout = 6000) {
    if (!this.connected) return Promise.reject(new Error('HTTP is disconnected.'));
    if (!['/state', '/set', '/stop', '/mode'].includes(path))
      return Promise.reject(new Error('Unknown command path.'));
    return new Promise((resolve, reject) => {
      if (path === '/stop') {
        const keep = [];
        for (const item of this.queue) {
          if (['/set', '/mode'].includes(item.path)) item.reject(new Error('Queued action cancelled by Stop.'));
          else keep.push(item);
        }
        this.queue = keep;
      }
      const item = {path, params, timeout, resolve, reject};
      if (path === '/stop') this.queue.unshift(item);
      else this.queue.push(item);
      this.pump();
    });
  }
  async pump() {
    if (this.active || !this.queue.length || !this.connected) return;
    const item = this.queue.shift();
    this.active = item;
    item.controller = new AbortController();
    const timer = setTimeout(() => item.controller.abort(), item.timeout);
    try {
      const reading = ['/state'].includes(item.path);
      const response = await fetch(this.address + item.path, {
        method: reading ? 'GET' : 'POST',
        body: reading ? undefined : new URLSearchParams(item.params),
        cache: 'no-store', signal: item.controller.signal,
      });
      const body = await response.text();
      if (!response.ok) throw new Error(body || 'HTTP ' + response.status);
      if (!this.connected || item.controller.signal.aborted) throw new Error('Connection closed.');
      item.resolve(JSON.parse(body));
    } catch (error) {
      item.reject(new Error(error.name === 'AbortError' || error instanceof TypeError ?
        'HTTP action not confirmed. It may have been applied; check state before retrying. Check address and browser local-network permissions.' : error.message));
    } finally {
      clearTimeout(timer);
      this.active = null;
      this.pump();
    }
  }
  async close() {
    this.address = null;
    this.active?.controller.abort();
    for (const item of this.queue.splice(0)) item.reject(new Error('HTTP disconnected.'));
  }
}
