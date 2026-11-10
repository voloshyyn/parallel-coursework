# Пошукова система з паралельною побудовою індексу та інтеграцією LLM

Курсова робота з дисципліни «Технології паралельних обчислень». Мета — дослідити та
реалізувати паралельні обчислення на прикладі пошукової системи: паралельна побудова
інвертованого індексу, потокобезпечна структура даних, паралельна обробка запитів і
розширення запиту локальною моделлю Ollama.

## Стек

| Область | Рішення |
|---|---|
| Бекенд | C++20, CMake |
| HTTP / WebSocket | Crow |
| HTTP-клієнт до Ollama | cpp-httplib |
| JSON | nlohmann/json |
| Тести | GoogleTest, ThreadSanitizer (`ENABLE_TSAN`) |
| LLM | Ollama, модель `qwen2.5:1.5b` |
| Web UI | Vue 3 через CDN, без збирання |
| CLI | окрема програма `search` на C++ |
| Розгортання | Docker, Docker Compose |
| UML | PlantUML, `docs/diagrams/` |

## Структура репозиторію

```
src/                 серверний код
  core/              токенізатор, шардований індекс, thread pool, пошук
  services/          IndexService: паралельна побудова і публікація індексу
  integrations/      клієнт Ollama
tools/               підготовка датасету
scripts/             допоміжні скрипти (smoke-тест)
docs/                специфікація проєкту
data/                датасет (не комітиться)
```

## Модель LLM

Обрано `qwen2.5:1.5b`:

- 1,5 млрд параметрів — у межах вимоги 1–3B;
- добре дотримується інструкцій, тому придатна для формування списку синонімів;
- підтримує багатомовність (українська та англійська), що важливо для запитів і
  англомовного корпусу;
- квантована версія займає близько 1 ГБ і працює на CPU без GPU.

Назва моделі задається змінною середовища `OLLAMA_MODEL`.

## Вимоги

- Docker 24+ з Docker Compose;
- Python 3.10+ для підготовки датасету;
- для локального збирання: CMake 3.22+, GCC 13+ або Clang 16+, `libasio-dev`.

## Підготовка датасету

Датасет у git не зберігається. Сирі книги лежать у `data/raw/train/` (5 322 файли
Project Gutenberg). Скрипт перевіряє їхню кількість і розмір, вирізає службові
блоки Gutenberg і нарізає книги на документи приблизно по 200 КіБ:

```bash
python tools/prepare_dataset.py
```

Результат — `data/docs/<book_id>_<n>.txt`. Скрипт друкує лише статистику.

## Запуск

```bash
docker compose up --build -d
```

Завантаження моделі виконується один раз (том `ollama_models` зберігає її між
запусками):

```bash
docker compose exec ollama ollama pull qwen2.5:1.5b
```

Перевірка:

```bash
curl http://localhost:8080/api/health
# {"ollama":true,"status":"ok"}

./scripts/smoke.sh
```

`ollama` у відповіді дорівнює `false`, якщо сервер Ollama недоступний; сам
`/api/health` у цьому разі все одно повертає 200.

## Використання API

Побудова індексу (читає весь датасет; `threads` — 1..64, типово 4):

```bash
curl -X POST http://localhost:8080/api/index \
  -H 'Content-Type: application/json' -d '{"threads": 4}'
# {"documents_indexed": 12061, "status": "ok", "time_ms": 40579}
```

Статистика індексу:

```bash
curl http://localhost:8080/api/stats
# {"documents": 12061, "index_build_time_ms": 40579, "threads_used": 4}
```

Пошук за ключовими словами:

```bash
curl -X POST http://localhost:8080/api/search \
  -H 'Content-Type: application/json' -d '{"query": "whale", "mode": "keyword"}'
# {"results": [{"doc_id": "...", "score": 1.0, "snippet": "..."}], "time_ms": 44}
```

Значення вище виміряні на повному датасеті (12 061 документ, 1,96 ГБ) у Docker
на 12-ядерній машині з `threads: 4`: побудова — 40,6 с, пошук — 44–48 мс.
Це не результати лабораторного експерименту (він буде на етапі 9), а приклад
відповіді.

Пошук до побудови індексу повертає 409 `index not built`, під час побудови —
409 `index is building`; `threads` поза 1..64 — 400. Повний контракт — у
`docs/api.md`.

## Локальне збирання

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/search_server
```

Конфігурація — через змінні середовища: `PORT`, `DATA_DIR`, `OLLAMA_URL`,
`OLLAMA_MODEL`.

## Тести

Тести збираються за допомогою GoogleTest і вмикаються опцією `BUILD_TESTING`
(за замовчуванням увімкнена через `CTest`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Образ сервера збирається без тестів (`-DBUILD_TESTING=OFF`). Для Linux-контейнера
передбачено збирання з ThreadSanitizer:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
```

## Документація

- `docs/api.md` — контракт REST і WebSocket;
- `docs/architecture.md` — архітектура та потокобезпека;
- `docs/deployment.md` — розгортання;
- `docs/experiments.md` — методика експериментів;
- `docs/testing.md` — план тестування;
- `docs/ui.md` — CLI та Web UI;
- `docs/conventions.md` — угоди щодо коду й комітів.
