# Пошукова система з паралельною побудовою індексу та LLM

Курсова робота з «Технологій паралельних обчислень»: C++20, Crow, шардований
інвертований індекс, **власний ThreadPool**, пошук TF-IDF та Ollama.
Web UI використовує локально поставлений Vue 3.5.13; інтернет під час роботи
інтерфейсу та підготовки датасету не потрібен.

## Запуск із чистого клону

Встановіть Docker з Compose, клонуйте репозиторій і виконайте в його каталозі:

```bash
docker compose up --build -d
```

Відкрийте http://localhost:8080. За замовчуванням створюється синтетичний корпус:
12 000 документів, щонайменше 2 GiB справжнього тексту. Генератор використовує
лише Python stdlib, фіксований seed та локальний словник; нічого не завантажує.
Індексація починається автоматично, викликати `/api/index` вручну не потрібно.
Генерація відбувається до запуску HTTP; її прогрес видно в `docker compose logs -f server`.
Після запуску HTTP сторінка показує стан індексації та сама дозволяє пошук,
коли індекс готовий. Відсутність Ollama не заважає keyword-пошуку.

Перший старт не миттєвий: потрібні генерація та індексація. Початкова збірка
образу й завантаження бібліотек можуть потребувати мережу. Подальша робота
пошуку та підготовка корпусу від неї не залежать.

Для швидкої демонстрації можна вибрати невеликий корпус:

```bash
# Bash
DATASET_PROFILE=demo docker compose up --build -d
```

```powershell
# PowerShell
$env:DATASET_PROFILE = 'demo'
docker compose up --build -d
```

Demo має 60 документів і **не виконує вимогу повного обсягу**. Full та demo
зберігаються окремо в named volume (`/data/generated/full` і `.../demo`).
Повторний запуск перевіряє manifest і використовує файли повторно, але індекс
у пам'яті будується заново. Невідомий або пошкоджений каталог не перезаписується.

## Перевірка та API

```bash
curl http://localhost:8080/api/health
# {"status":"ok","ollama":false}
curl http://localhost:8080/api/ready
# Додатковий маршрут: search_ready, index_state, index_error
curl http://localhost:8080/api/stats
curl -X POST http://localhost:8080/api/search \
  -H 'Content-Type: application/json' -d '{"query":"coral reef","mode":"keyword"}'
./scripts/smoke.sh
```

Smoke чекає автоматичної готовності; для окремої перевірки ручної перебудови
встановіть `REBUILD_INDEX=true`. WebSocket `/ws/search` надсилає
`query_expansion` одразу після розширення, потім `keyword_search` і `done`.
Повний контракт — [docs/api.md](docs/api.md).

## Ollama (необов'язково)

```bash
docker compose --profile llm up -d
docker compose exec ollama ollama pull qwen2.5:1.5b
```

Модель завантажується окремо один раз у том `ollama_models`. За недоступності
моделі/сервера або deadline генерації 5 с пошук використовує вихідний запит.
`ollama` у health означає доступність сервера Ollama, а не наявність моделі.

## Власний датасет

Існуючий Gutenberg-корпус залишається альтернативою. Його не змінює генератор.
Монтуйте каталог `.txt` read-only через `docker-compose.external.yml`, задавши
`EXTERNAL_DATASET_PATH` (докладно — [docs/deployment.md](docs/deployment.md)).
`tools/prepare_dataset.py` збережено для наявних 5 322 сирих книг; він не потрібен
для стандартного запуску. У git не потрапляють корпуси, моделі та збірки.

## Збирання й тести

Локально потрібні CMake 3.22+, GCC 13+/Clang 16+, Asio, Python 3.10+ і Node 18+.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Повний тестовий образ включає також JavaScript- і Python-перевірки:

```bash
docker build --target test -t coursework-tests .
docker run --rm coursework-tests
```

ThreadSanitizer: `-DENABLE_TSAN=ON`, докладно — [docs/testing.md](docs/testing.md).
Для прямого запуску C++-бінарника спочатку створіть корпус генератором і задайте
`DATA_DIR`; `AUTO_INDEX=true` за замовчуванням.

## Реалізація та конфігурація

`src/core/thread_pool.*` створює свої `std::thread`, чергу задач, mutex і
condition_variable; готових сторонніх пулів немає. Цей пул використовується
індексатором і фоновим WebSocket-пошуком. Закриття чекає прийняті задачі;
частковий збій створення потоків обробляється без terminate.

Налаштування: `PORT`, `DATA_DIR`, `AUTO_INDEX`, `INDEX_THREADS` (1..64),
`SEARCH_WORKERS` (1..64), `SEARCH_TASK_QUEUE`, `SEARCH_CONNECTION_QUEUE`,
`OLLAMA_URL`, `OLLAMA_MODEL`. У Compose також є `DATASET_PROFILE` (`full`/`demo`).

Документація: [архітектура](docs/architecture.md), [розгортання](docs/deployment.md),
[тести](docs/testing.md), [експерименти](docs/experiments.md), [UI](docs/ui.md).
