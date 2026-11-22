'use strict';

// Thin Vue wrapper around search_client.js. The client owns the state machine
// (sockets, generation ids, watchdog, protocol validation); this file only
// renders plain strings. Vue is served from the binary, so no CDN is involved.

(function () {
  const root = document.getElementById('app');
  const showFatal = function (message) {
    if (!root) {
      return;
    }
    root.removeAttribute('v-cloak');
    root.textContent = message;
  };

  if (typeof Vue === 'undefined') {
    showFatal(
      'Не вдалося завантажити локальний Vue. Перевірте, що /vue.global.prod.js віддається сервером.',
    );
    return;
  }
  if (!window.SearchClient) {
    showFatal(
      'Не вдалося завантажити клієнт пошуку. Перевірте, що /search_client.js віддається сервером.',
    );
    return;
  }

  const STAGES = window.SearchClient.STAGES;
  const STATE_LABELS = {
    pending: 'очікування',
    running: 'виконується',
    done: 'готово',
    error: 'помилка',
  };
  const INDEX_LABELS = {
    empty: 'не побудований',
    building: 'будується…',
    ready: 'готовий',
    error: 'помилка',
    unknown: '—',
  };

  const freshStages = function () {
    return STAGES.map(function (stage) {
      return { id: stage.id, label: stage.label, state: 'pending' };
    });
  };

  const initialUi = function () {
    return {
      query: '',
      status: 'idle',
      stages: freshStages(),
      expansion: null,
      results: [],
      searchTimeMs: null,
      error: '',
      healthChecked: false,
      serverAvailable: false,
      ollamaAvailable: false,
      indexState: 'unknown',
      indexError: '',
      searchReady: false,
    };
  };

  const requestTimeoutMs = (function () {
    const fromAttribute = Number(root && root.dataset.requestTimeoutMs);
    if (Number.isFinite(fromAttribute) && fromAttribute >= 1000) {
      return fromAttribute;
    }
    const fromWindow = Number(window.SEARCH_REQUEST_TIMEOUT_MS);
    if (Number.isFinite(fromWindow) && fromWindow >= 1000) {
      return fromWindow;
    }
    return 60000;
  })();

  Vue.createApp({
    data() {
      return {
        ui: initialUi(),
        client: null,
      };
    },
    computed: {
      healthText() {
        if (!this.ui.healthChecked) return 'перевіряю…';
        return this.ui.serverAvailable ? 'доступний' : 'недоступний';
      },
      ollamaText() {
        if (!this.ui.healthChecked || !this.ui.serverAvailable) return '—';
        return this.ui.ollamaAvailable ? 'доступна' : 'недоступна';
      },
      indexText() {
        if (!this.ui.healthChecked || !this.ui.serverAvailable) return '—';
        if (this.ui.indexState === 'empty' && this.ui.indexError) {
          return 'помилка';
        }
        return INDEX_LABELS[this.ui.indexState] || this.ui.indexState;
      },
      healthClass() {
        if (!this.ui.healthChecked) return 'is-unknown';
        return this.ui.serverAvailable ? 'is-ok' : 'is-down';
      },
      preparing() {
        return (
          this.ui.serverAvailable &&
          !this.ui.searchReady &&
          this.ui.indexState !== 'ready'
        );
      },
      preparingText() {
        if (this.ui.indexState === 'building') {
          return 'Індекс будується. Пошук стане доступним автоматично.';
        }
        if (this.ui.indexError) {
          return 'Індекс не вдалося побудувати: ' + this.ui.indexError;
        }
        if (this.ui.indexState === 'empty') {
          return 'Індекс ще не побудований. Сервер підготує його автоматично.';
        }
        return 'Сервер готує індекс.';
      },
      searching() {
        return this.ui.status === 'searching';
      },
      showEmpty() {
        return this.ui.status === 'done' && this.ui.results.length === 0;
      },
      canSearch() {
        return (
          !this.searching &&
          this.ui.searchReady &&
          this.ui.query.trim().length > 0
        );
      },
    },
    mounted() {
      this.client = window.SearchClient.createSearchClient({
        requestTimeoutMs: requestTimeoutMs,
        onChange: (state) => {
          this.ui = state;
        },
      });
      this.ui = this.client.snapshot();
      this.client.startHealthPolling();
    },
    beforeUnmount() {
      if (this.client) {
        this.client.dispose();
        this.client = null;
      }
    },
    methods: {
      stateText(state) {
        return STATE_LABELS[state] || state;
      },
      onQuery(event) {
        if (this.client) {
          this.client.setQuery(event.target.value);
        }
      },
      search() {
        if (this.client) {
          this.client.search();
        }
      },
    },
  }).mount('#app');
})();
