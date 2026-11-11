# Testing

Автоматична перевірка дає до 60 балів, тож тести мають відтворювати критерії силабуса.

## Чекліст критеріїв (приймальні перевірки)

| # | Критерій (бали) | Перевірка |
|---|---|---|
| 1 | Docker Compose (5) | `docker compose up`, потім `GET /api/health` → 200 |
| 2 | Інвертований індекс (5) | `POST /api/index` будує індекс; `GET /api/stats` показує `documents` |
| 3 | Keyword search (5) | `POST /api/search` з `mode=keyword` повертає JSON у заданому форматі |
| 4 | Thread pool (5) | `threads` приймається; `time_ms` зменшується при збільшенні потоків |
| 5 | Паралельна структура (5) | Стрес-тест без втрати даних і гонок |
| 6 | Query expansion (5) | Запит розширюється через Ollama `/api/generate` |
| 7 | WebSocket (5) | `/ws/search` віддає `query_expansion` → `keyword_search` → `done` |
| 8 | CLI (5) | `./search "запит"` друкує результати |
| 9 | Web UI (5) | Форма, результати, візуалізація етапів |
| 10 | README + чистий код (5) | Інструкція відтворюється з нуля; коміти за Conventional Commits |
| 11 | Масштабованість (10) | Графіки і аналіз з `experiments.md` |

## Юніт-тести
- **ThreadPool:** виконання всіх задач, завершення (shutdown) без втрат, поведінка при винятку в задачі, 1 потік і багато потоків.
- **InvertedIndex:** додавання/читання, однакові результати для 1 і N потоків, граничні випадки (порожній документ, повторні слова).
- **Tokenizer:** регістр, пунктуація, стоп-слова (якщо є).
- **Ollama client:** розбір відповіді; таймаут і fallback на оригінальний запит (з мок-сервером).
- **Ранжування:** детермінований порядок для однакових score.

Файли: `tests/thread_pool_test.cpp`, `tests/tokenizer_test.cpp`,
`tests/inverted_index_test.cpp`, `tests/search_engine_test.cpp`,
`tests/index_service_test.cpp`, `tests/api_test.cpp`,
`tests/ollama_client_test.cpp`, `tests/search_pipeline_test.cpp`,
`tests/ws_search_test.cpp`, `tests/ws_route_test.cpp`. Для індексу перевіряються,
зокрема, збіг postings послідовної і паралельної побудови, запис із 8 потоків,
читання під час запису та однаковий результат за 1 і 64 шардів.

REST перевіряється на справжньому сервері Crow (порт з фіксованим номером,
клієнт — cpp-httplib): коди 200/400/409, схема JSON, `threads` за замовчуванням,
пошук до побудови і під час побудови, порядок і нормування `score`. Побудова під
час цих тестів іде в тимчасовий каталог із кількох файлів, датасет не потрібен.

WebSocket перевіряється двічі. Протокол — на фейковому
`crow::websocket::connection`, який записує надіслане: порядок етапів, поля
`original`/`expanded_terms`/`results`/`time_ms`, `done` без `data`, одна помилка
без закриття, повторний запит після помилки, відповіді до побудови і під час
побудови індексу. Маршрут — на справжньому сервері з мінімальним клієнтом
RFC 6455 (handshake, масковані текстові кадри): перевіряються upgrade `101` і
послідовність трьох повідомлень по дроту.

## Тести конкурентності
- Стрес: багато потоків пишуть в індекс одночасно, порівняти результат з послідовною побудовою (ті самі терміни та postings).
- Читання під час запису: пошук під час `POST /api/index` не падає і не повертає пошкоджені дані.
- Інструменти: ThreadSanitizer (C++) / аналог для обраної мови; повторювати тести багато разів.
- Deadlock: тест із таймаутом на завершення.

## Інтеграційні тести
- Усі REST-ендпоінти: статуси, схема JSON (збіг із `api.md`).
- Негативні випадки: невалідний JSON, пошук до індексації, невідомий `mode`.
- WebSocket: порядок і зміст повідомлень, помилка без закриття з'єднання,
  повторний запит у тому самому з'єднанні.
- Кілька клієнтів одночасно (REST і WebSocket).
- Робота без Ollama: `health` повертає `ollama: false`, пошук не падає.

## Тести пошуку (для звіту)
Таблиця «запит / очікуваний / фактичний результат» ведеться в `experiments.md`.

## Запуск

Звичайна збірка з тестами:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Кожен тестовий бінарник у CTest має жорсткий таймаут 120 с, тому взаємне блокування
падає як помилка, а не зависає.

### ThreadSanitizer

На Windows TSan у WSL зазвичай завершується з `unexpected memory mapping`, тому
збираємо і запускаємо його в Linux-контейнері. `--security-opt seccomp=unconfined`
потрібен sanitizer-ам, а `setarch -R` вимикає рандомізацію адресного простору:

```bash
docker run --rm --security-opt seccomp=unconfined \
  -v "$PWD":/src:ro ubuntu:24.04 bash -c '
    set -euo pipefail
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
      build-essential cmake git ca-certificates libasio-dev
    cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_TSAN=ON -DBUILD_TESTING=ON
    cmake --build /tmp/build -j
    setarch -R ctest --test-dir /tmp/build --output-on-failure
  '
```

### Smoke-тест

`scripts/smoke.sh` чекає, доки `GET /api/health` не поверне 200, будує індекс
(`POST /api/index`, `threads: 2`), перевіряє `GET /api/stats` і виконує
`POST /api/search`. Скрипт друкує лише коди, кількість документів/результатів і
`time_ms`, без тексту документів. Час на побудову обмежений
`BUILD_TIMEOUT_SECONDS` (типово 900 с).
