# Розгортання

## Стандартний старт

`docker compose up` піднімає `server`, `ollama` і одноразовий `model-init`.
Після зміни коду виконати `docker compose up --build -d`.
Не потрібні ручні завантаження книг, pip, підготовка data/raw, ollama pull
або POST /api/index. Початковій збірці й отриманню моделі потрібна мережа.
Генерація корпусу та робота вебінтерфейсу мережі не потребують.

## Готовність

`tools/start_server.py` запускає C++ HTTP-сервер одразу, готує корпус у фоні
й автоматично викликає /api/index. UI показує підготовку і дозволяє пошук після
готовності. HTTP доступний під час генерації. На першому старті пошук по
повному корпусу потребує часу на створення текстів та індексацію.

`GET /api/health` — liveness, фіксовані status/ollama, завжди 200.
`GET /api/ready` — index_state/search_ready/index_error, а в Docker також
startup_state/startup_error. Помилка генератора зберігається й показується UI;
сервер лишається доступним для діагностики. Docker healthcheck перевіряє HTTP,
а для готовності пошуку використовувати /api/ready або scripts/smoke.sh.

Supervisor передає SIGTERM/SIGINT серверу й генератору, чекає завершення та
прибирає дочірні процеси. Startup status записується атомарною заміною файла
у /tmp; файли корпусу й модель не використовуються як тимчасові файли стану.

## Корпус

`full` (типовий): 12 000 .txt, >=2 GiB реального UTF-8 тексту.
`demo`: 60 малих документів, лише швидка демонстрація, не для атестації обсягу.
DATASET_PROFILE=demo обирає demo. Кожний профіль має власний каталог у томі.

Генератор пише staging-каталог на тому самому filesystem і перейменовує його
в /data/generated/<profile> лише після успіху. Named volume змонтовано в
батьківський /data/generated, а не в каталог, що перейменовується. Власник тому —
непривілейований app (uid 10001). Чужі непорожні дані не замінюються.
Невідповідний manifest дає помилку; для іншого seed/версії вибрати новий каталог.
DATASET_VERIFY_HASH=true додатково перевіряє SHA-256, а звичайне повторне
використання перевіряє назви, кількість, розмір, seed, профіль і версію.

Контрольний full: seed=20240517, 12 000 документів, 2 462 028 386 байтів,
SHA-256: 6bf997ceac0124e0fdd7f8de32209611d24372cc64bdedff24fee11d7ef8637f.
Це синтетичні тематичні тексти, не справжні книги. Пошукові та performance-висновки
не можна переносити на Gutenberg без додаткових вимірювань. Відомі запити —
у manifest: coral reef, quantum physics, ancient rome та інші.
Індекс у RAM перебудовується при перезапуску; тексти в томі використовуються знову.

Для наявного корпусу задати EXTERNAL_DATASET_PATH абсолютним шляхом і виконати:

```bash
docker compose -f docker-compose.yml -f docker-compose.external.yml up --build -d
```

Override задає GENERATE_DATASET=false і монтує зовнішній каталог read-only.
Індексатор читає лише .txt, без manifest та службових файлів.

## Ollama та модель

Ollama запускається завжди, без окремого профілю. model-init чекає /api/tags,
завантажує OLLAMA_MODEL лише за відсутності, потім прогріває через /api/generate
з keep_alive=24h. Дані в ollama_models повторно використовуються. При помилці
ініціалізація повторюється до трьох разів; причина видима в docker compose logs
model-init. У разі проблем із мережею сервер і keyword fallback доступні.

`ollama:true` у health означає доступність API й наявність обраної моделі.
Це не гарантує успіху кожної генерації: query expansion має total deadline 5 с
та fallback. Прогрів зменшує ризик timeout на завантаженні моделі.

Порти хоста можна змінити SEARCH_PORT / OLLAMA_PORT; типово 8080 / 11434.
При зміні OLLAMA_MODEL повторити звичайний compose up; model-init перевірить
нову модель. Для вимірювань keyword-throughput є QUERY_EXPANSION=false,
в нормальній роботі воно true. Протокол запуску — bench/results/verification.json.
