'use strict';

// The page is a thin client for the API: /api/health for the indicator and
// /ws/search for the search itself. Vue comes from a CDN, so every user value
// is rendered with text interpolation, which escapes HTML.

(function () {
  const STAGES = [
    { id: 'query_expansion', label: 'Розширення запиту (Ollama)' },
    { id: 'keyword_search', label: 'Пошук за ключовими словами' },
    { id: 'done', label: 'Готово' },
  ];

  const HEALTH_INTERVAL_MS = 5000;
  const STATE_LABELS = {
    pending: 'очікування',
    running: 'виконується',
    done: 'готово',
    error: 'помилка',
  };

  const root = document.getElementById('app');
  if (typeof Vue === 'undefined') {
    root.textContent =
      'Не вдалося завантажити Vue з CDN. Перевірте доступ до інтернету.';
    return;
  }

  const freshStages = () =>
    STAGES.map((stage) => ({ id: stage.id, label: stage.label, state: 'pending' }));

  Vue.createApp({
    data() {
      return {
        query: '',
        searching: false,
        stages: freshStages(),
        expansion: null,
        results: [],
        searchTimeMs: null,
        finished: false,
        error: '',
        healthChecked: false,
        serverAvailable: false,
        ollamaAvailable: false,
        socket: null,
        healthTimer: null,
        // A socket we close after done must not be reported as a failure.
        closingOnPurpose: false,
      };
    },
    computed: {
      healthText() {
        if (!this.healthChecked) return 'перевіряю…';
        return this.serverAvailable ? 'доступний' : 'недоступний';
      },
      ollamaText() {
        if (!this.healthChecked || !this.serverAvailable) return '—';
        return this.ollamaAvailable ? 'доступна' : 'недоступна';
      },
      healthClass() {
        if (!this.healthChecked) return 'is-unknown';
        return this.serverAvailable ? 'is-ok' : 'is-down';
      },
    },
    mounted() {
      this.checkHealth();
      this.healthTimer = window.setInterval(
        () => this.checkHealth(),
        HEALTH_INTERVAL_MS,
      );
    },
    beforeUnmount() {
      window.clearInterval(this.healthTimer);
      this.closeSocket();
    },
    methods: {
      stateText(state) {
        return STATE_LABELS[state] || state;
      },
      async checkHealth() {
        try {
          const response = await fetch('/api/health', { cache: 'no-store' });
          const body = await response.json();
          this.serverAvailable = response.ok;
          this.ollamaAvailable = body.ollama === true;
        } catch (error) {
          this.serverAvailable = false;
          this.ollamaAvailable = false;
        }
        this.healthChecked = true;
      },
      setStage(id, state) {
        const stage = this.stages.find((item) => item.id === id);
        if (stage) stage.state = state;
      },
      failRemaining(detail) {
        for (const stage of this.stages) {
          if (stage.state === 'pending' || stage.state === 'running') {
            stage.state = 'error';
          }
        }
        this.error = detail;
        this.finished = true;
        this.searching = false;
      },
      closeSocket() {
        if (!this.socket) return;
        this.closingOnPurpose = true;
        // A normal close (1000) makes Crow echo a valid frame; closing without
        // a code makes the browser complain about a reserved status code.
        this.socket.close(1000);
        this.socket = null;
      },
      search() {
        const query = this.query.trim();
        if (!query || this.searching) return;

        this.closeSocket();
        this.closingOnPurpose = false;
        this.searching = true;
        this.finished = false;
        this.error = '';
        this.expansion = null;
        this.results = [];
        this.searchTimeMs = null;
        this.stages = freshStages();

        const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
        const socket = new WebSocket(
          scheme + '://' + window.location.host + '/ws/search',
        );
        this.socket = socket;
        this.setStage('query_expansion', 'running');

        socket.addEventListener('open', () => {
          socket.send(JSON.stringify({ query: query }));
        });

        socket.addEventListener('message', (event) => {
          let message;
          try {
            message = JSON.parse(event.data);
          } catch (parseError) {
            this.failRemaining('Сервер надіслав некоректне повідомлення.');
            this.closeSocket();
            return;
          }
          this.handleMessage(message);
        });

        socket.addEventListener('error', () => {
          if (this.closingOnPurpose) return;
          this.failRemaining('Не вдалося з\u2019єднатися з сервером через WebSocket.');
        });

        socket.addEventListener('close', () => {
          if (this.closingOnPurpose) return;
          if (this.searching) {
            this.failRemaining('З\u2019єднання закрито до завершення пошуку.');
          }
        });
      },
      handleMessage(message) {
        if (message.stage === 'query_expansion') {
          this.expansion = message.data || { original: '', expanded_terms: [] };
          this.setStage('query_expansion', 'done');
          this.setStage('keyword_search', 'running');
          return;
        }
        if (message.stage === 'keyword_search') {
          const data = message.data || {};
          this.results = Array.isArray(data.results) ? data.results : [];
          this.searchTimeMs = typeof data.time_ms === 'number' ? data.time_ms : null;
          this.setStage('keyword_search', 'done');
          this.setStage('done', 'running');
          return;
        }
        if (message.stage === 'done') {
          this.setStage('done', 'done');
          this.finished = true;
          this.searching = false;
          this.closeSocket();
          return;
        }
        if (message.stage === 'error') {
          const detail =
            message.data && message.data.message
              ? message.data.message
              : 'невідома помилка';
          this.failRemaining('Помилка: ' + detail);
          this.closeSocket();
        }
      },
    },
  }).mount('#app');
})();
