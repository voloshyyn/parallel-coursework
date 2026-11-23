# Пошукова система з паралельними обчисленнями та LLM

Курсова робота: C++20, **власні ThreadPool та шардований InvertedIndex**, REST,
WebSocket, C++ CLI, локальний Vue та Ollama. Готові бібліотечні реалізації
пулу потоків або пошукового індексу не використовуються.

## Запуск

Потрібен Docker з Compose. У каталозі клонованого репозиторію:

```bash
docker compose up
```

Або у фоні з явним оновленням образу після зміни коду:

```bash
docker compose up --build -d
```

Відкрити [http://localhost:8080](http://localhost:8080).
За замовчуванням запускаються `server`, `ollama` і одноразовий `model-init`.
Останній автоматично завантажує відсутню `qwen2.5:1.5b` і прогріває її.
Модель та корпус зберігаються в постійних named volumes.

Сторінка доступна **до завершення підготовки корпусу**. Сервер у фоні створює
12 000 англомовних синтетичних документів обсягом >=2 GiB, потім автоматично
індексує їх. UI сам дозволяє пошук після готовності. Помилка підготовки видима
на сторінці та в `/api/ready`; прогрес генерації — в логах:

```bash
docker compose logs -f server model-init
```

Генерація текстів не потребує мережі, pip або скачування книг. Початкові образи,
C++-залежності та модель потребують мережі. Пошук по повному корпусу не може
завершити підготовку миттєво; її час залежить від CPU й диска. Подальші запуски
повторно використовують тексти та модель; RAM-індекс перебудовується.
За timeout/недоступності LLM пошук використовує початковий запит.

## Консольний клієнт

C++ клієнт уже входить у runtime-образ:

```bash
docker compose exec server search "coral reef"
docker compose exec server search --json "quantum physics"
```

Він друкує `doc_id`, `score`, `snippet` та час keyword-етапу.
Після локального збирання: `./build/search "coral reef"`.
Адреса: `--server http://localhost:8080` або `SEARCH_URL`.
Коди завершення: 0 — успіх, включно з порожнім результатом; 1 — помилка
з'єднання/сервера/протоколу; 2 — неправильні аргументи. `--help` показує довідку.

Відомі запити full-корпусу: `coral reef`, `quantum physics`, `ancient rome`,
`deep ocean whale`, `galaxy telescope`. Це синтетичні тексти, не справжні книги.

## API

Усі відповіді — JSON (`Content-Type: application/json`). Адреса за замовчуванням
`http://localhost:8080`. Типи: `string`, `integer`, `number` (дробове),
`boolean`, `array`, `object`.

```bash
curl http://localhost:8080/api/health
curl -X POST http://localhost:8080/api/index -H 'Content-Type: application/json' \
  -d '{"threads":8}'
curl -X POST http://localhost:8080/api/search -H 'Content-Type: application/json' \
  -d '{"query":"coral reef","mode":"keyword"}'
curl http://localhost:8080/api/stats
```

### `GET /api/health`

Запиту немає. Відповідь `200`:

| Поле | Тип | Опис |
|---|---|---|
| `status` | string | Завжди `"ok"` |
| `ollama` | boolean | `true`, якщо Ollama доступна й має вибрану модель (оновлюється фоново раз на 5 с) |

### `POST /api/index` — побудова індексу

Запит:

| Поле | Тип | Обов'язкове | Опис |
|---|---|---|---|
| `threads` | integer, 1–64 | ні (типово 4) | Кількість потоків власного пулу |

Відповідь `200`:

| Поле | Тип | Опис |
|---|---|---|
| `status` | string | `"ok"` |
| `documents_indexed` | integer | Кількість проіндексованих документів |
| `time_ms` | integer | Час побудови, мс |

### `POST /api/search` — пошук

Запит:

| Поле | Тип | Обов'язкове | Опис |
|---|---|---|---|
| `query` | string | так | Непорожній текст запиту |
| `mode` | string | так | Лише `"keyword"` |

Відповідь `200`:

| Поле | Тип | Опис |
|---|---|---|
| `results` | array of object | До 10 елементів, за спаданням `score` |
| `results[].doc_id` | string | Ім'я файла документа |
| `results[].score` | number | Нормована оцінка TF-IDF, (0, 1]; перший результат має `1` |
| `results[].snippet` | string | Фрагмент тексту навколо знайденого терміна |
| `time_ms` | integer | Час keyword-етапу, мс |

### `GET /api/stats`

Відповідь `200` (до першої побудови — нулі):

| Поле | Тип | Опис |
|---|---|---|
| `documents` | integer | Кількість документів в індексі |
| `index_build_time_ms` | integer | Час останньої успішної побудови, мс |
| `threads_used` | integer | Потоків у цій побудові |

### `GET /api/ready` — додатковий маршрут готовності

| Поле | Тип | Опис |
|---|---|---|
| `status` | string | `"ok"` |
| `ollama` | boolean | Як у `/api/health` |
| `index_state` | string | `"empty"`, `"building"` або `"ready"` |
| `search_ready` | boolean | `true`, коли пошук доступний |
| `index_error` | string | Причина невдалої побудови або `""` |
| `startup_state` | string | Лише в Docker: `"preparing"`, `"indexing"`, `"ready"`, `"error"` |
| `startup_error` | string | Лише в Docker: причина помилки підготовки або `""` |

### Помилки

Тіло помилки: `{"status": "error", "message": string}`.

| HTTP | Коли | `message` |
|---|---|---|
| 400 | Некоректний JSON; порожній або не рядковий `query`; `mode` не `"keyword"`; `threads` не ціле в 1–64 | `invalid JSON body`, `query must be a non-empty string`, `mode must be "keyword"`, `threads must be in 1..64` |
| 409 | Пошук до першої побудови; пошук або повторна побудова під час побудови | `index not built`, `index is building` |
| 404 / 405 | Невідомий маршрут / недозволений метод | текст `404 Not Found` / `405 Method Not Allowed` |
| 500 | Внутрішня помилка (наприклад, недоступний каталог датасету) | текст помилки |

### WebSocket `/ws/search`

Клієнт надсилає текстове JSON-повідомлення; після `done` з'єднання лишається
відкритим для наступного запиту.

| Напрям | Повідомлення |
|---|---|
| Клієнт → сервер | `{"query": string}` |
| Сервер → клієнт | `{"stage": "query_expansion", "data": {"original": string, "expanded_terms": array of string}}` |
| Сервер → клієнт | `{"stage": "keyword_search", "data": {"results": array of object, "time_ms": integer}}` (елементи `results` — як у `/api/search`) |
| Сервер → клієнт | `{"stage": "done"}` (без `data`) |
| Сервер → клієнт | `{"stage": "error", "data": {"message": string}}` — замість подальших етапів; `done` після помилки не надсилається |

Повідомлення `message` помилок: `invalid JSON body`, `query must be a non-empty
string`, `index not built`, `index is building`, `server is busy`. Якщо Ollama
недоступна, `expanded_terms` порожній, а пошук іде за оригінальним запитом.
Докладніше: [docs/api.md](docs/api.md).

## Дані та конфігурація

`full` — типовий профіль для вимог курсової: 12 000 .txt, >=2 GiB.
`demo` — 60 малих документів лише для швидкої демонстрації; не для атестації
обсягу та вимірювань. Зміна профілю не перезаписує інший корпус.

```bash
DATASET_PROFILE=demo docker compose up --build -d
```

У PowerShell: `$env:DATASET_PROFILE='demo'`, потім звичайна команда Compose.
Для повернення full: `Remove-Item Env:DATASET_PROFILE`.

Налаштування Compose: `INDEX_THREADS` (типово 4), `SEARCH_WORKERS` (4),
`SEARCH_TASK_QUEUE` (64), `SEARCH_CONNECTION_QUEUE` (4), `OLLAMA_MODEL`,
`SEARCH_PORT` (8080), `OLLAMA_PORT` (11434).
`QUERY_EXPANSION=false` потрібне лише для ізольованих keyword-експериментів;
у звичайній роботі значення `true`.

### Датасет: шлях і формат

- **Типовий шлях:** `/data/generated/full` у контейнері `server` (named volume
  `generated_data`); профіль `demo` — `/data/generated/demo`. Змінна `DATA_DIR`.
- **Формат:** плоский каталог звичайних текстових файлів `*.txt` у кодуванні
  UTF-8, один документ — один файл (у цій роботі ~200 КіБ). Файли з іншими
  розширеннями та підкаталоги ігноруються. Ідентифікатор `doc_id` у відповідях —
  ім'я файла (наприклад, `doc-000123.txt`).
- **Вимоги курсової:** англомовний текст, не менше 10 000 документів.
- **Синтетичний корпус** створюється автоматично (`tools/generate_dataset.py`,
  зерно фіксоване, manifest із SHA-256).
- **Справжні тексти (Project Gutenberg):** `python tools/prepare_dataset.py`
  розрізає книги з `data/raw/train/` на файли `<book_id>_<n>.txt` у `data/docs/`;
  підключаються через `EXTERNAL_DATASET_PATH` (нижче).

Для власних .txt задати `EXTERNAL_DATASET_PATH` абсолютним шляхом та виконати:

```bash
docker compose -f docker-compose.yml -f docker-compose.external.yml up --build -d
```

Каталог монтується read-only, генерація вимикається. Подробиці:
[docs/deployment.md](docs/deployment.md).

## Збирання та перевірки

Без Docker потрібні C++20-компілятор, CMake >=3.22, Asio та Git:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

CMake отримує зафіксовані версії Crow, cpp-httplib, nlohmann/json і GoogleTest.
Node та Python потрібні для відповідних suites. Єдиний стиль C++ задано
`.clang-format` (Google).

Відтворювана перевірка у Linux з усіма залежностями тестів:

```bash
docker build --target test -t coursework-tests .
docker run --rm coursework-tests
```

Свіжий протокол: [docs/verification.md](docs/verification.md).
Команди ThreadSanitizer та опис перевірок: [docs/testing.md](docs/testing.md).

## Результати й пояснювальна записка

- [PDF-звіт українською](output/pdf/coursework_report.pdf).
- [UML: PlantUML-джерела та PNG](docs/diagrams/).
- [Архітектура](docs/architecture.md).
- [Методика та відтворення експериментів](bench/README.md).
- [Сирі вимірювання й параметри середовища](bench/results/).
- [Графіки PDF, SVG, PNG](bench/plots/).
- [Результати та аналіз масштабування](docs/experiments.md).

Для регенерації графіків і звіту потрібні Python, `reportlab` та шрифт Times New
Roman (або сумісний Liberation Serif / DejaVu Serif); `pypdfium2` додатково
створює PNG графіків. Звіт оформлено за вимогами: Times New Roman 14 пт,
інтервал 1,5, поля 30/10/20/20 мм:

```bash
python tools/plot_results.py
python tools/build_report.py --author "Andrii Voloshyn"
```

Параметр `--group` додає групу на титульний лист. Звіт читає CSV та фактичний
протокол перевірки; числа не підставляються вручну. Код, UML, звіт і результати
зберігаються в Git; корпуси, моделі та програмні build-артефакти — поза Git.
