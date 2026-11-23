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

```bash
curl http://localhost:8080/api/health
curl http://localhost:8080/api/ready
curl http://localhost:8080/api/stats
curl -X POST http://localhost:8080/api/search -H 'Content-Type: application/json' \
  -d '{"query":"coral reef","mode":"keyword"}'
curl -X POST http://localhost:8080/api/index -H 'Content-Type: application/json' \
  -d '{"threads":8}'
```

`/api/health` повертає фіксовані `status` і `ollama`; останнє означає доступність
Ollama та наявність вибраної моделі. `/api/ready` окремо повідомляє готовність
індексу й помилки фонового старту. WebSocket `/ws/search` приймає `{ "query":
"coral reef" }` і передає `query_expansion`, `keyword_search`, `done`.
Контракт: [docs/api.md](docs/api.md).

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
