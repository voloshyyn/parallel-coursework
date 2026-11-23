# API

Фіксований контракт з Додатка А силабуса. Змінювати назви ендпоінтів і поля не можна: перевірка автоматична.
Усі відповіді — JSON.

## REST

### GET /api/health
Перевірка стану сервера.

Відповідь `200`:
```json
{ "status": "ok", "ollama": true }
```
`ollama` — `false`, якщо Ollama недоступна або вибраної моделі немає в `/api/tags`
(сервер при цьому має відповідати 200). Наявність моделі не є гарантією
успіху кожної генерації; timeout усе одно застосовується.
Значення `ollama` оновлюється фоновою перевіркою раз на 5 секунд: ендпоінт відповідає
одразу з кешованим станом і не чекає на мережу.

### GET /api/ready (додатковий маршрут)

Окремий маршрут для готовності пошуку; фіксований контракт `/api/health`
залишається без додаткових полів. HTTP `200` означає, що сервер відповідає;
`search_ready` показує, чи готовий індекс:

```json
{"status":"ok","ollama":false,"index_state":"building","search_ready":false,"index_error":""}
```

`index_state`: `empty`, `building`, `ready`; `index_error` містить причину
невдалої побудови. UI опитує цей маршрут кожні 5 с з таймаутом 4 с.
У Docker додатково є `startup_state` (`preparing`, `indexing`, `ready`, `error`)
та `startup_error`. Вони описують фонову підготовку корпусу й автоматичну
індексацію. При її помилці HTTP та сторінка залишаються доступними.

### POST /api/index
Побудова індексу з текстових файлів датасету.

Запит:
```json
{ "threads": 4 }
```
Відповідь:
```json
{ "status": "ok", "documents_indexed": 2000, "time_ms": 1234 }
```

### POST /api/search
Пошук за ключовими словами.

Запит:
```json
{ "query": "text", "mode": "keyword" }
```
Відповідь:
```json
{ "results": [{ "doc_id": "...", "score": 0.95, "snippet": "..." }], "time_ms": 56 }
```

### GET /api/stats
Статистика індексу.

Відповідь:
```json
{ "documents": 2000, "index_build_time_ms": 1234, "threads_used": 4 }
```

## WebSocket /ws/search

Клієнт надсилає:
```json
{ "query": "search text" }
```

Сервер надсилає по одному JSON-повідомленню на етап, у порядку:
```json
{ "stage": "query_expansion", "data": { "original": "...", "expanded_terms": ["..."] } }
{ "stage": "keyword_search", "data": { "results": [], "time_ms": 12 } }
{ "stage": "done" }
```

Порядок фіксований; `done` не має поля `data`. Після `done` з'єднання
лишається відкритим, тож клієнт може надіслати наступний запит у те саме
з'єднання.

Розширення надсилається одразу після Ollama, до завершення keyword-пошуку.
Запити одного з'єднання обслуговуються послідовно, включно з помилками
валідації. При переповненні черги повертається `server is busy`; поточна
партія цього з'єднання скасовується, відкладені результати та `done` після
цієї помилки не надсилаються. Наступний запит можливий після звільнення worker.

Помилка (наша домовленість, у специфікації її немає): якщо запит обробити не
можна — некоректний JSON, порожній `query`, індекс не побудований або будується,
внутрішня помилка — сервер надсилає одне повідомлення
`{ "stage": "error", "data": { "message": "..." } }` і не надсилає `done`.
Тексти `message` ті самі, що в REST: `invalid JSON body`,
`query must be a non-empty string`, `index not built`, `index is building`.

## Помилки (наші домовленості, у специфікації їх немає)

| Ситуація | HTTP | Тіло |
|---|---|---|
| Некоректний JSON / відсутнє поле | 400 | `{ "status": "error", "message": "..." }` |
| Пошук до побудови індексу або під час побудови | 409 | `{ "status": "error", "message": "index not built" }` або `"index is building"` |
| Повторний `POST /api/index` під час побудови | 409 | `{ "status": "error", "message": "index is building" }` |
| Внутрішня помилка (наприклад, недоступний каталог датасету) | 500 | `{ "status": "error", "message": "..." }` |
| Ollama недоступна під час пошуку | — | пошук продовжується з оригінальним запитом, `expanded_terms` порожній |

`GET /api/stats` до першої побудови повертає `200` з нулями:
`{ "documents": 0, "index_build_time_ms": 0, "threads_used": 0 }`.

`POST /api/search` зараз підтримує лише `mode: "keyword"`; інше значення або
відсутнє поле — `400`. Порожній `query` — теж `400`. `results` містить не
більше 10 елементів, `score` нормований у (0, 1], порядок детермінований
(див. `architecture.md`).

## Правила `threads`
- `threads` < 1 або не ціле число → `400` з `message: "threads must be in 1..64"`.
- `threads` > 64 → той самий `400`.
- Якщо поле `threads` відсутнє, беремо значення за замовчуванням `4`.

## Приклади curl

```bash
curl http://localhost:8080/api/health
curl -X POST http://localhost:8080/api/index -H "Content-Type: application/json" -d '{"threads": 4}'
curl http://localhost:8080/api/stats
curl -X POST http://localhost:8080/api/search -H "Content-Type: application/json" -d '{"query": "text", "mode": "keyword"}'
```
