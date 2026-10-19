// Direct browser USB transport for the existing MC newline/JSON protocol.
export class SerialTransport extends EventTarget {
  constructor() {
    super();
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.active = null;
    this.queue = [];
    this.sequence = crypto.getRandomValues(new Uint32Array(1))[0] % 999999999;
    this.closing = false;
  }

  get connected() { return !!this.port && !this.closing; }

  async open(port) {
    if (this.port) throw new Error('A USB connection is already open.');
    this.closing = false;
    try {
      await port.open({baudRate: 115200, bufferSize: 4096, flowControl: 'none'});
      this.port = port;
      // ESP32 USB adapters can reset on open. Initial read-only handshake retries handle boot.
      await port.setSignals({dataTerminalReady: false, requestToSend: false});
      this.writer = port.writable.getWriter();
      this.readTask = this.readLoop(port);
    } catch (error) {
      if (this.port) await this.close();
      throw error;
    }
  }

  request(path, params = {}, timeout = 6000) {
    if (!this.connected) return Promise.reject(new Error('USB is not connected.'));
    return new Promise((resolve, reject) => {
      if (path === '/stop') {
        // Do not allow an older queued Apply to enable an output after Stop.
        const keep = [];
        for (const item of this.queue) {
          if (['/set', '/mode'].includes(item.path)) {
            item.reject(new Error('Queued action cancelled by Stop.'));
          } else keep.push(item);
        }
        this.queue = keep;
      }
      const item = {path, params, timeout, resolve, reject};
      if (path === '/stop') this.queue.unshift(item);
      else this.queue.push(item);
      this.pump();
    });
  }

  pump() {
    if (this.active || !this.queue.length || !this.connected) return;
    const item = this.queue.shift();
    this.sequence = this.sequence % 999999999 + 1;
    const query = new URLSearchParams(item.params).toString();
    const command = new TextEncoder().encode(`MC ${this.sequence} ${item.path}${query ? '?' + query : ''}\n`);
    if (command.byteLength > 1537) {
      item.reject(new Error('Command exceeds the firmware limit of 1536 bytes.'));
      this.pump(); return;
    }
    item.id = this.sequence;
    this.active = item;
    item.timer = setTimeout(() => {
      this.finish(item, new Error('USB request timed out. The command may have been applied; check device status before retrying.'));
    }, item.timeout);
    this.writer.write(command).catch(error => this.finish(item, error));
  }

  finish(item, error, data) {
    if (this.active !== item) return;
    clearTimeout(item.timer);
    this.active = null;
    if (error) item.reject(error);
    else item.resolve(data);
    this.pump();
  }

  async readLoop(port) {
    let buffer = '';
    const decoder = new TextDecoder();
    try {
      this.reader = port.readable.getReader();
      while (!this.closing) {
        const {value, done} = await this.reader.read();
        if (done) break;
        buffer += decoder.decode(value, {stream: true});
        let newline;
        while ((newline = buffer.indexOf('\n')) >= 0) {
          const line = buffer.slice(0, newline).trimEnd();
          buffer = buffer.slice(newline + 1);
          if (!line.startsWith('MC ')) continue;
          let reply;
          try { reply = JSON.parse(line.slice(3)); } catch { continue; }
          const item = this.active;
          if (!item || reply?.id !== item.id) continue; // Ignore stale replies and boot messages.
          if (reply.code !== 200) this.finish(item, new Error(String(reply.data || 'Command rejected.')));
          else if (!Object.hasOwn(reply, 'data')) this.finish(item, new Error('Incomplete firmware response.'));
          else this.finish(item, null, reply.data);
        }
        if (buffer.length > 16384) buffer = '';
      }
    } catch (error) {
      if (!this.closing) this.readError = error;
    } finally {
      this.reader?.releaseLock();
      this.reader = null;
      if (!this.closing) {
        this.dispatchEvent(new Event('disconnect'));
        // Run cleanup after this read task resolves to avoid waiting on itself.
        queueMicrotask(() => this.close());
      }
    }
  }

  async close() {
    if (this.closing) return this.closeTask;
    this.closing = true;
    this.closeTask = this.cleanup();
    return this.closeTask;
  }

  async cleanup() {
    const error = new Error('USB disconnected. An in-flight command may have been applied.');
    if (this.active) this.finish(this.active, error);
    for (const item of this.queue.splice(0)) item.reject(error);
    try { await this.reader?.cancel(); } catch { /* Cable may already be disconnected. */ }
    try { await this.readTask; } catch { /* Reported by readLoop. */ }
    this.writer?.releaseLock();
    this.writer = null;
    try { await this.port?.close(); } catch { /* Device may already be closed. */ }
    this.port = null;
  }
}
