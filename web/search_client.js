'use strict';

// Framework-free search state machine. app.js renders it with Vue; Node tests
// drive it with a fake WebSocket and fake timers, so WebSocket races, protocol
// validation and the watchdog are tested as behaviour, not as substrings.
//
// Everything the server sends is treated as untrusted data: the client keeps
// plain strings and never builds HTML.

(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.SearchClient = factory();
  }
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  const STAGES = [
    { id: 'query_expansion', label: 'Розширення запиту (Ollama)' },
    { id: 'keyword_search', label: 'Пошук за ключовими словами' },
    { id: 'done', label: 'Готово' },
  ];

  const STATE_LABELS = {
    pending: 'очікування',
    running: 'виконується',
    done: 'готово',
    error: 'помилка',
  };

  const PROTOCOL_ERROR = 'Сервер надіслав некоректне повідомлення.';
  const DISCONNECT_ERROR = 'З\u2019єднання закрито до завершення пошуку.';
  const SOCKET_ERROR =
    'Не вдалося з\u2019єднатися з сервером через WebSocket.';
  const TIMEOUT_ERROR = 'Перевищено час очікування відповіді.';

  function freshStages() {
    return STAGES.map(function (stage) {
      return { id: stage.id, label: stage.label, state: 'pending' };
    });
  }

  function isPlainObject(value) {
    return value !== null && typeof value === 'object' && !Array.isArray(value);
  }

  function isFiniteNumber(value) {
    return typeof value === 'number' && Number.isFinite(value);
  }

  function validHit(hit) {
    return (
      isPlainObject(hit) &&
      typeof hit.doc_id === 'string' &&
      isFiniteNumber(hit.score) &&
      typeof hit.snippet === 'string'
    );
  }

  // Returns null for a valid message, otherwise a short reason (for tests and
  // for a fixed user-facing protocol error).
  function validateMessage(message) {
    if (!isPlainObject(message)) {
      return 'message must be an object';
    }
    if (typeof message.stage !== 'string') {
      return 'stage must be a string';
    }
    if (message.stage === 'query_expansion') {
      if (!isPlainObject(message.data)) {
        return 'query_expansion data must be an object';
      }
      if (typeof message.data.original !== 'string') {
        return 'original must be a string';
      }
      if (
        !Array.isArray(message.data.expanded_terms) ||
        !message.data.expanded_terms.every(function (term) {
          return typeof term === 'string';
        })
      ) {
        return 'expanded_terms must be an array of strings';
      }
      return null;
    }
    if (message.stage === 'keyword_search') {
      if (!isPlainObject(message.data)) {
        return 'keyword_search data must be an object';
      }
      if (
        !Array.isArray(message.data.results) ||
        !message.data.results.every(validHit)
      ) {
        return 'results must be an array of hits';
      }
      if (
        message.data.time_ms !== undefined &&
        !isFiniteNumber(message.data.time_ms)
      ) {
        return 'time_ms must be a number';
      }
      return null;
    }
    if (message.stage === 'done') {
      return null;
    }
    if (message.stage === 'error') {
      if (
        !isPlainObject(message.data) ||
        typeof message.data.message !== 'string'
      ) {
        return 'error data must contain a message';
      }
      return null;
    }
    return 'unknown stage';
  }

  function createSearchClient(options) {
    const opts = Object.assign(
      {
        url: '/ws/search',
        healthUrl: '/api/ready',
        requestTimeoutMs: 60000,
        healthTimeoutMs: 4000,
        healthIntervalMs: 5000,
        WebSocketImpl:
          typeof WebSocket !== 'undefined' ? WebSocket : undefined,
        fetchImpl:
          typeof fetch !== 'undefined' ? fetch.bind(globalThis) : undefined,
        setTimeoutImpl: function (fn, ms) {
          return setTimeout(fn, ms);
        },
        clearTimeoutImpl: function (id) {
          clearTimeout(id);
        },
        AbortControllerImpl:
          typeof AbortController !== 'undefined' ? AbortController : undefined,
        onChange: function () {},
      },
      options || {},
    );

    const state = {
      query: '',
      status: 'idle', // idle | searching | done | error
      stages: freshStages(),
      expansion: null,
      results: [],
      searchTimeMs: null,
      error: '',
      requestId: 0,
      healthChecked: false,
      healthInFlight: false,
      serverAvailable: false,
      ollamaAvailable: false,
      indexState: 'unknown',
      indexError: '',
      searchReady: false,
    };

    let socket = null;
    let requestTimer = null;
    let healthTimer = null;
    let healthController = null;
    let disposed = false;

    function snapshot() {
      return Object.assign({}, state, {
        stages: state.stages.map(function (stage) {
          return Object.assign({}, stage);
        }),
        results: state.results.map(function (hit) {
          return Object.assign({}, hit);
        }),
        expansion: state.expansion
          ? {
              original: state.expansion.original,
              expanded_terms: state.expansion.expanded_terms.slice(),
            }
          : null,
      });
    }

    function emit() {
      opts.onChange(snapshot());
    }

    function setStage(id, value) {
      for (const stage of state.stages) {
        if (stage.id === id) {
          stage.state = value;
          return;
        }
      }
    }

    function stopRequestTimer() {
      if (requestTimer !== null) {
        opts.clearTimeoutImpl(requestTimer);
        requestTimer = null;
      }
    }

    function closeSocket(target) {
      if (!target) {
        return;
      }
      if (socket === target) {
        socket = null;
      }
      try {
        target.close(1000);
      } catch (error) {
        // Already closed by the peer; nothing to do.
      }
    }

    function fail(message) {
      if (state.status !== 'searching') {
        return;
      }
      for (const stage of state.stages) {
        if (stage.state === 'pending' || stage.state === 'running') {
          stage.state = 'error';
        }
      }
      state.status = 'error';
      state.error = message;
      stopRequestTimer();
      closeSocket(socket);
      emit();
    }

    function finish() {
      setStage('done', 'done');
      state.status = 'done';
      state.error = '';
      stopRequestTimer();
      closeSocket(socket);
      emit();
    }

    function handleMessage(raw, isCurrent) {
      let message;
      try {
        message = JSON.parse(raw);
      } catch (error) {
        fail(PROTOCOL_ERROR);
        return;
      }
      if (validateMessage(message) !== null) {
        fail(PROTOCOL_ERROR);
        return;
      }
      if (!isCurrent()) {
        return;
      }
      if (message.stage === 'query_expansion') {
        state.expansion = {
          original: message.data.original,
          expanded_terms: message.data.expanded_terms.slice(),
        };
        setStage('query_expansion', 'done');
        setStage('keyword_search', 'running');
        emit();
        return;
      }
      if (message.stage === 'keyword_search') {
        state.results = message.data.results.map(function (hit) {
          return {
            doc_id: hit.doc_id,
            score: hit.score,
            snippet: hit.snippet,
          };
        });
        state.searchTimeMs = isFiniteNumber(message.data.time_ms)
          ? message.data.time_ms
          : null;
        setStage('keyword_search', 'done');
        setStage('done', 'running');
        emit();
        return;
      }
      if (message.stage === 'done') {
        finish();
        return;
      }
      if (message.stage === 'error') {
        fail('Помилка: ' + message.data.message);
      }
    }

    function search() {
      const query = state.query.trim();
      if (!query || state.status === 'searching' || disposed) {
        return false;
      }
      if (!opts.WebSocketImpl) {
        state.status = 'searching';
        fail('WebSocket недоступний у цьому браузері.');
        return false;
      }

      const id = ++state.requestId;
      // A previous request (done or failed) is closed before a new one, and
      // its events stop being current because the id and socket change.
      closeSocket(socket);
      stopRequestTimer();
      state.status = 'searching';
      state.error = '';
      state.expansion = null;
      state.results = [];
      state.searchTimeMs = null;
      state.stages = freshStages();
      setStage('query_expansion', 'running');
      emit();

      let connection;
      try {
        connection = new opts.WebSocketImpl(opts.url);
      } catch (error) {
        fail(SOCKET_ERROR);
        return false;
      }
      socket = connection;

      const isCurrent = function () {
        return id === state.requestId && socket === connection;
      };

      connection.addEventListener('open', function () {
        if (!isCurrent()) {
          closeSocket(connection);
          return;
        }
        try {
          connection.send(JSON.stringify({ query: query }));
        } catch (error) {
          fail(SOCKET_ERROR);
        }
      });

      connection.addEventListener('message', function (event) {
        if (!isCurrent()) {
          return;
        }
        handleMessage(event.data, isCurrent);
      });

      connection.addEventListener('error', function () {
        if (!isCurrent()) {
          return;
        }
        fail(SOCKET_ERROR);
      });

      connection.addEventListener('close', function () {
        if (!isCurrent()) {
          return;
        }
        if (state.status === 'searching') {
          fail(DISCONNECT_ERROR);
        }
      });

      requestTimer = opts.setTimeoutImpl(function () {
        if (!isCurrent()) {
          return;
        }
        fail(TIMEOUT_ERROR);
      }, opts.requestTimeoutMs);

      return true;
    }

    function setQuery(value) {
      state.query = typeof value === 'string' ? value : '';
      emit();
    }

    function applyHealth(body, responseOk) {
      const ok = responseOk && isPlainObject(body);
      state.serverAvailable = ok;
      if (!ok) {
        state.ollamaAvailable = false;
        state.indexState = 'unknown';
        state.indexError = '';
        state.searchReady = false;
        return;
      }
      state.ollamaAvailable = body.ollama === true;
      state.indexState =
        typeof body.index_state === 'string' ? body.index_state : 'ready';
      state.indexError =
        typeof body.index_error === 'string' ? body.index_error : '';
      if (typeof body.startup_error === 'string' && body.startup_error) {
        state.indexError = body.startup_error;
      }
      if (typeof body.search_ready === 'boolean') {
        state.searchReady = body.search_ready;
      } else {
        // Older servers only had status and ollama; assume search is possible.
        state.searchReady = state.indexState !== 'building';
      }
    }

    async function checkHealth() {
      if (state.healthInFlight || disposed || !opts.fetchImpl) {
        return false;
      }
      state.healthInFlight = true;
      emit();

      let controller = null;
      let timeoutId = null;
      if (opts.AbortControllerImpl) {
        controller = new opts.AbortControllerImpl();
        healthController = controller;
        timeoutId = opts.setTimeoutImpl(function () {
          try {
            controller.abort();
          } catch (error) {
            // Abort is best effort.
          }
        }, opts.healthTimeoutMs);
      }

      try {
        const response = await opts.fetchImpl(opts.healthUrl, {
          cache: 'no-store',
          signal: controller ? controller.signal : undefined,
        });
        let body = null;
        try {
          body = await response.json();
        } catch (error) {
          body = null;
        }
        applyHealth(body, response.ok);
      } catch (error) {
        applyHealth(null, false);
      } finally {
        if (timeoutId !== null) {
          opts.clearTimeoutImpl(timeoutId);
        }
        if (healthController === controller) {
          healthController = null;
        }
        state.healthInFlight = false;
        state.healthChecked = true;
        emit();
      }
      return true;
    }

    function startHealthPolling() {
      if (healthTimer !== null) {
        return;
      }
      checkHealth();
      healthTimer = setInterval(function () {
        checkHealth();
      }, opts.healthIntervalMs);
    }

    function stopHealthPolling() {
      if (healthTimer !== null) {
        clearInterval(healthTimer);
        healthTimer = null;
      }
      if (healthController) {
        try {
          healthController.abort();
        } catch (error) {
          // Ignore.
        }
        healthController = null;
      }
    }

    function dispose() {
      disposed = true;
      stopRequestTimer();
      closeSocket(socket);
      stopHealthPolling();
    }

    return {
      state: state,
      snapshot: snapshot,
      setQuery: setQuery,
      search: search,
      checkHealth: checkHealth,
      startHealthPolling: startHealthPolling,
      stopHealthPolling: stopHealthPolling,
      dispose: dispose,
      stateText: function (value) {
        return STATE_LABELS[value] || value;
      },
    };
  }

  return {
    STAGES: STAGES,
    createSearchClient: createSearchClient,
    validateMessage: validateMessage,
  };
});
