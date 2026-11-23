# Відтворення експериментів

Python-скрипт використовує лише стандартну бібліотеку. Запускати з кореня
репозиторію після збирання `coursework-search:local` через Compose.
Потрібен вже підготовлений **full** корпус, не demo.

## Окремий сервер

Показаний том належить типовому Compose-проєкту; перевірити його назву через
`docker volume ls`, якщо Compose запущено з іншим `-p`.

```bash
docker run -d --name coursework-bench -p 18090:8080 \
  -e DATA_DIR=/data/generated/full -e GENERATE_DATASET=false \
  -e QUERY_EXPANSION=false -e OLLAMA_URL=http://127.0.0.1:9 \
  -v course_work_parallel_computing_generated_data:/data/generated:ro \
  coursework-search:local
curl http://localhost:18090/api/ready
```

Дочекатися `search_ready:true`. Окремий контейнер виключає LLM з E2; типовий
Compose продовжує використовувати LLM. Не запускати компіляцію або інше важке
навантаження одночасно із замірами. Якщо недостатньо RAM для двох повних
індексів, перед вимірюваннями зупинити звичайний server.

```bash
python bench/run.py --threads 1,2,4,8,12,16 --clients 1,2,4,8,16,32,64 \
  --repetitions 5 --requests 256
python tools/plot_results.py
docker stop coursework-bench
```

`--output` задає інший каталог, щоб зберегти попередні результати. Типовий
`bench/results` перезаписується новим дослідом. Кількість потоків підбирається
під CPU машини; включити одну точку понад число логічних CPU.

## Методика та файли

- E1: повна перебудова того самого індексу; `time_ms` сервера і HTTP wall time.
- E2: окремий keyword-сервер, 256 запитів на повторення, нове TCP-з'єднання
  на кожний запит, задана кількість потоків-клієнтів. Облік помилок обов'язковий.
- Один неврахований прогрів перед кожною точкою; потім п'ять запусків.
- Точки на графіках — медіани, вуса — min-max. Кеш ОС прогрітий, не очищується.
- p50/p95 обчислюються в кожному повторенні, потім береться медіана п'яти
  повторень. Це не percentile усіх об'єднаних запитів.
- `environment.json`: CPU, RAM, Docker, ліміти контейнера, kernel, образ,
  seed/manifest корпусу, набір запитів і стан Git на момент вимірювання.
- `e1.csv`, `e2.csv`: усі виміряні повторення, включно з повільними.
- `search_cases.json`: очікування і фактичні doc_id, score, snippet.
- `summary.json`: похідні медіани, прискорення та ефективність.
- `verification.json`: окремий функціональний протокол, не performance-дані.

У фінальному досліді 23.11.2026 використано Linux named volume
`course_work_parallel_computing_generated_data` з full-корпусом. Параметри конкретного
запуску записані в `environment.json`; назва тому не впливає на зміст даних.

Графіки створюються з CSV, без ручного підбору чисел. Для Python-рендерера
потрібні `reportlab`, Times New Roman (або Liberation Serif / DejaVu Serif), опційно `pypdfium2` для PNG.
PDF/SVG є векторними експортами. Проаналізовані обмеження та результати:
`docs/experiments.md`, розділ 4 PDF-звіту.
