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

## Локальне збирання

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/search_server
```

Конфігурація — через змінні середовища: `PORT`, `DATA_DIR`, `OLLAMA_URL`,
`OLLAMA_MODEL`.

## Тести

Юніт- та інтеграційні тести додаються на наступних етапах. Для Linux-контейнера
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
