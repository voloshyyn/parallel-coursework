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
`ollama` — `false`, якщо Ollama недоступна (сервер при цьому має відповідати 200).

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

## Помилки (наші домовленості, у специфікації їх немає)

| Ситуація | HTTP | Тіло |
|---|---|---|
| Некоректний JSON / відсутнє поле | 400 | `{ "status": "error", "message": "..." }` |
| Пошук до побудови індексу або під час побудови | 409 | `{ "status": "error", "message": "index not built" }` або `"index is building"` |
| Ollama недоступна під час пошуку | — | пошук продовжується з оригінальним запитом, `expanded_terms` порожній |

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
