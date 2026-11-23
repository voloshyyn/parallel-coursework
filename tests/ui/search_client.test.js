'use strict';

// Behavioural tests for web/search_client.js. They drive a fake WebSocket and
// fake timers, so races, protocol errors, the watchdog and the health probe are
// checked as state transitions rather than as substrings in the HTML.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const {
  createSearchClient,
  validateMessage,
} = require('../../web/search_client.js');

const SOURCE = fs.readFileSync(
  path.join(__dirname, '..', '..', 'web', 'search_client.js'),
  'utf8',
);

function makeSocketClass() {
  class FakeSocket {
    constructor(url) {
      this.url = url;
      this.listeners = new Map();
      this.sent = [];
      this.closedWith = null;
      FakeSocket.instances.push(this);
    }

    addEventListener(type, handler) {
      if (!this.listeners.has(type)) {
        this.listeners.set(type, []);
      }
      this.listeners.get(type).push(handler);
    }

    send(data) {
      this.sent.push(data);
    }

    close(code) {
      this.closedWith = code;
    }

    emit(type, event) {
      for (const handler of this.listeners.get(type) || []) {
        handler(event || {});
      }
    }
  }
  FakeSocket.instances = [];
  return FakeSocket;
}

function makeTimers() {
  let nextId = 1;
  const pending = new Map();
  return {
    setTimeout(handler) {
      const id = nextId;
      nextId += 1;
      pending.set(id, handler);
      return id;
    },
    clearTimeout(id) {
      pending.delete(id);
    },
    fireAll() {
      const handlers = Array.from(pending.values());
      pending.clear();
      for (const handler of handlers) {
        handler();
      }
    },
    count() {
      return pending.size;
    },
  };
}

function FakeAbortController() {
  const listeners = [];
  this.signal = {
    addEventListener(type, handler) {
      if (type === 'abort') {
        listeners.push(handler);
      }
    },
  };
  this.abort = function () {
    for (const listener of listeners) {
      listener();
    }
  };
}

function makeClient(overrides) {
  const options = Object.assign(
    {
      WebSocketImpl: makeSocketClass(),
      setTimeoutImpl: function () {
        return 0;
      },
      clearTimeoutImpl: function () {},
      AbortControllerImpl: FakeAbortController,
      fetchImpl: async function () {
        return { ok: true, json: async () => ({}) };
      },
      requestTimeoutMs: 1000,
      healthTimeoutMs: 1000,
    },
    overrides || {},
  );
  return createSearchClient(options);
}

function openSearch(client, sockets, query) {
  client.setQuery(query);
  assert.equal(client.search(), true);
  const socket = sockets.instances[sockets.instances.length - 1];
  socket.emit('open');
  return socket;
}

function message(stage, data) {
  return { data: JSON.stringify(data === undefined ? { stage } : { stage, data }) };
}

test('successful search updates stages, results and closes the socket', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
  });

  client.setQuery('fish');
  assert.equal(client.search(), true);
  assert.equal(sockets.instances.length, 1);
  const socket = sockets.instances[0];
  assert.equal(socket.url, '/ws/search');
  assert.equal(timers.count(), 1);

  socket.emit('open');
  assert.deepEqual(JSON.parse(socket.sent[0]), { query: 'fish' });

  socket.emit(
    'message',
    message('query_expansion', {
      original: 'fish',
      expanded_terms: ['whale'],
    }),
  );
  assert.equal(client.state.status, 'searching');
  assert.equal(client.state.stages[0].state, 'done');
  assert.equal(client.state.stages[1].state, 'running');
  assert.deepEqual(client.state.expansion.expanded_terms, ['whale']);

  socket.emit(
    'message',
    message('keyword_search', {
      results: [{ doc_id: 'a.txt', score: 1, snippet: 'fish here' }],
      time_ms: 12,
    }),
  );
  assert.equal(client.state.results.length, 1);
  assert.equal(client.state.searchTimeMs, 12);

  socket.emit('message', message('done'));
  assert.equal(client.state.status, 'done');
  assert.equal(client.state.error, '');
  assert.equal(client.state.stages[2].state, 'done');
  assert.equal(timers.count(), 0);
  assert.equal(socket.closedWith, 1000);
});

test('a successful empty result is not an error', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
  });
  const socket = openSearch(client, sockets, 'nothing');

  socket.emit(
    'message',
    message('query_expansion', { original: 'nothing', expanded_terms: [] }),
  );
  socket.emit(
    'message',
    message('keyword_search', { results: [], time_ms: 3 }),
  );
  socket.emit('message', message('done'));

  assert.equal(client.state.status, 'done');
  assert.equal(client.state.results.length, 0);
  assert.equal(client.state.error, '');
});

test('an error stage leaves a reusable state and closes only this socket', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
  });
  const socket = openSearch(client, sockets, 'fish');

  socket.emit(
    'message',
    message('error', { message: 'index not built' }),
  );

  assert.equal(client.state.status, 'error');
  assert.match(client.state.error, /index not built/);
  assert.equal(client.state.stages[0].state, 'error');
  assert.equal(timers.count(), 0);
  assert.equal(socket.closedWith, 1000);
  // The form is usable again: a second search starts cleanly.
  assert.equal(client.search(), true);
  assert.equal(sockets.instances.length, 2);
});

test('JSON null and malformed schemas are protocol errors, not crashes', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
  });

  const socket = openSearch(client, sockets, 'fish');
  assert.doesNotThrow(() => socket.emit('message', { data: 'null' }));
  assert.equal(client.state.status, 'error');
  assert.match(client.state.error, /некоректне/);
  assert.equal(socket.closedWith, 1000);

  assert.equal(validateMessage(null), 'message must be an object');
  assert.equal(validateMessage([]), 'message must be an object');
  assert.equal(validateMessage({ stage: 'done' }), null);
  assert.notEqual(
    validateMessage({ stage: 'keyword_search', data: { results: [{}] } }),
    null,
  );
});

test('a stale socket event cannot change the new search', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
  });

  const first = openSearch(client, sockets, 'A');
  first.emit(
    'message',
    message('query_expansion', { original: 'A', expanded_terms: [] }),
  );
  first.emit('message', message('keyword_search', { results: [] }));
  first.emit('message', message('done'));
  assert.equal(client.state.status, 'done');

  client.setQuery('B');
  assert.equal(client.search(), true);
  const second = sockets.instances[1];
  assert.equal(first.closedWith, 1000);

  first.emit('close');
  assert.equal(client.state.status, 'searching');
  first.emit('error');
  assert.equal(client.state.status, 'searching');
  first.emit('message', message('error', { message: 'stale' }));
  assert.equal(client.state.status, 'searching');
  first.emit('open');
  assert.equal(second.sent.length, 0);

  second.emit('open');
  assert.deepEqual(JSON.parse(second.sent[0]), { query: 'B' });
  assert.equal(client.state.status, 'searching');
});

test('the request watchdog frees the UI when no done arrives', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
    requestTimeoutMs: 1000,
  });
  const socket = openSearch(client, sockets, 'stuck');

  assert.equal(timers.count(), 1);
  timers.fireAll();

  assert.equal(client.state.status, 'error');
  assert.match(client.state.error, /Перевищено/);
  assert.equal(socket.closedWith, 1000);
  assert.equal(timers.count(), 0);
});

test('a repeated search starts a new generation and timer', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
  });
  const first = openSearch(client, sockets, 'one');
  first.emit(
    'message',
    message('query_expansion', { original: 'one', expanded_terms: [] }),
  );
  first.emit('message', message('keyword_search', { results: [] }));
  first.emit('message', message('done'));

  const second = openSearch(client, sockets, 'two');
  assert.equal(sockets.instances.length, 2);
  assert.notEqual(first, second);
  assert.equal(first.closedWith, 1000);
  assert.equal(timers.count(), 1);
  assert.deepEqual(JSON.parse(second.sent[0]), { query: 'two' });

  second.emit('message', message('done'));
  assert.equal(timers.count(), 0);
});

test('untrusted document text is stored as plain text, never as markup', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
  });
  const socket = openSearch(client, sockets, 'xss');

  const payload = '<img src=x onerror="alert(1)">';
  socket.emit(
    'message',
    message('keyword_search', {
      results: [
        {
          doc_id: '<script>alert(1)</script>',
          score: 1,
          snippet: payload,
        },
      ],
      time_ms: 1,
    }),
  );

  assert.equal(client.state.results[0].snippet, payload);
  assert.equal(client.state.results[0].doc_id, '<script>alert(1)</script>');
  assert.equal(typeof client.snapshot().results[0].snippet, 'string');
  assert.equal(SOURCE.includes('innerHTML'), false);
  assert.equal(SOURCE.includes('document.write'), false);
  assert.equal(SOURCE.includes('https://'), false);
});

test('the client only talks to same-origin endpoints', async () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const seen = [];
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
    fetchImpl: async function (url) {
      seen.push(url);
      return {
        ok: true,
        json: async () => ({
          status: 'ok',
          ollama: true,
          index_state: 'ready',
          search_ready: true,
          index_error: '',
        }),
      };
    },
  });

  const socket = openSearch(client, sockets, 'offline');
  assert.equal(socket.url, '/ws/search');

  assert.equal(await client.checkHealth(), true);
  assert.deepEqual(seen, ['/api/ready']);
  assert.equal(client.state.serverAvailable, true);
  assert.equal(client.state.indexState, 'ready');
  assert.equal(client.state.searchReady, true);
});

test('health checks do not overlap', async () => {
  const timers = makeTimers();
  let resolveFetch = null;
  let calls = 0;
  const client = makeClient({
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
    fetchImpl: function () {
      calls += 1;
      return new Promise((resolve) => {
        resolveFetch = resolve;
      });
    },
  });

  const first = client.checkHealth();
  const second = client.checkHealth();
  assert.equal(await second, false);
  assert.equal(calls, 1);

  resolveFetch({
    ok: true,
    json: async () => ({
      status: 'ok',
      ollama: false,
      index_state: 'building',
      search_ready: false,
    }),
  });
  assert.equal(await first, true);
  assert.equal(client.state.serverAvailable, true);
  assert.equal(client.state.indexState, 'building');
  assert.equal(client.state.searchReady, false);
  assert.equal(timers.count(), 0);
});

test('health fetch is aborted at a finite deadline', async () => {
  const timers = makeTimers();
  const client = makeClient({
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
    healthTimeoutMs: 25,
    fetchImpl: function (url, options) {
      return new Promise((resolve, reject) => {
        options.signal.addEventListener('abort', () => {
          reject(new Error('aborted'));
        });
      });
    },
  });

  const pending = client.checkHealth();
  timers.fireAll();
  assert.equal(await pending, true);
  assert.equal(client.state.serverAvailable, false);
  assert.equal(client.state.healthChecked, true);
  assert.equal(timers.count(), 0);
});

test('dispose cancels the timer and the socket', () => {
  const sockets = makeSocketClass();
  const timers = makeTimers();
  const client = makeClient({
    WebSocketImpl: sockets,
    setTimeoutImpl: timers.setTimeout,
    clearTimeoutImpl: timers.clearTimeout,
  });
  const socket = openSearch(client, sockets, 'bye');

  client.dispose();

  assert.equal(timers.count(), 0);
  assert.equal(socket.closedWith, 1000);
  // A disposed client refuses new work.
  client.setQuery('later');
  assert.equal(client.search(), false);
});

test('missing WebSocket is a visible error rather than a silent failure', () => {
  const client = makeClient({ WebSocketImpl: null });
  client.setQuery('alpha');
  assert.equal(client.search(), false);
  assert.equal(client.state.status, 'error');
  assert.match(client.state.error, /WebSocket/);
});

test('the no-Vue fallback removes cloak and shows a visible message', () => {
  const vm = require('node:vm');
  const appSource = fs.readFileSync(path.join(__dirname, '../../web/app.js'), 'utf8');
  let hidden = true;
  const root = {
    textContent: '',
    removeAttribute(name) { if (name === 'v-cloak') hidden = false; },
  };
  vm.runInNewContext(appSource, { document: { getElementById: () => root } });
  assert.equal(hidden, false);
  assert.match(root.textContent, /Vue/);
});

test('preparation failure is visible while the HTTP server stays available', async () => {
  const client = makeClient({
    fetchImpl: async () => ({ ok: true, json: async () => ({
      status: 'ok', ollama: false, index_state: 'empty', search_ready: false,
      startup_state: 'error', startup_error: 'dataset preparation failed',
    }) }),
  });
  await client.checkHealth();
  assert.equal(client.state.serverAvailable, true);
  assert.equal(client.state.searchReady, false);
  assert.match(client.state.indexError, /dataset preparation failed/);
});

test('the browser fetch receiver remains the global object', async () => {
  const vm = require('node:vm');
  const sandbox = vm.createContext({ setTimeout, clearTimeout, AbortController });
  vm.runInContext(`
    globalThis.fetch = function () {
      if (this !== globalThis) throw new TypeError('Illegal invocation');
      return Promise.resolve({ok:true, json: async () => ({
        status:'ok', ollama:false, index_state:'ready', search_ready:true
      })});
    };
  ` + SOURCE + '\nglobalThis.client = SearchClient.createSearchClient();', sandbox);
  await sandbox.client.checkHealth();
  assert.equal(sandbox.client.state.serverAvailable, true);
  assert.equal(sandbox.client.state.searchReady, true);
});
