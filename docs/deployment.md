# Deployment

Вимога: систему можна підняти однією командою.

```bash
docker compose up
```

Перша атестація (тижні 7–8): `docker compose up` піднімає систему, `GET /api/health` повертає 200.

## Сервіси

| Сервіс | Призначення | Порт |
|---|---|---|
| `server` | Пошуковий сервер (REST + WebSocket + статика Web UI) | 8080 |
| `ollama` | Локальна LLM для query expansion | 11434 |

## Вимоги до compose-файлу
- `server` залежить від `ollama` (`depends_on`), але `/api/health` має відповідати 200 і тоді, коли Ollama ще не готова (`"ollama": false`).
- Том для моделей Ollama, щоб не завантажувати їх заново при кожному запуску.
- Том (read-only) з датасетом, змонтований у контейнер `server`.
- Змінні середовища для `server`: `OLLAMA_URL=http://ollama:11434`, `OLLAMA_MODEL`, `DATA_DIR`, `PORT`.
- Healthcheck для `server`.

## Завантаження моделі Ollama
Після першого запуску потрібно завантажити модель (назву обрати в `stack.md`):

```bash
docker compose exec ollama ollama pull <model>
```

## Датасет
- Текстові файли англійською мовою, не менше 10 000 документів.
- Датасет видається на вводному занятті; дозволено власний або згенерований. Ми використовуємо книги Project Gutenberg з публічного бакета `deepmind-gutenberg` (Google Cloud Storage).
- Сирі дані: зріз `train/`, 5 322 книги, 1 999 124 802 байти. Лежать у `data/raw/train/`, у Git не потрапляють.
- Підготовка: `python tools/prepare_dataset.py` ріже книги на документи по ~200 КБ і пише їх у `data/docs/` (формат `<book_id>_<n>.txt`).
- Результат: 12 061 документ, 1 955 788 242 байти (≈1,96 ГБ). Вимога «не менше 10 000 документів» виконана; обсяг трохи нижче 2 ГБ, див. рішення про докачування.

## Перевірка після запуску
```bash
curl http://localhost:8080/api/health
curl -X POST http://localhost:8080/api/index -H "Content-Type: application/json" -d '{"threads": 4}'
curl http://localhost:8080/api/stats
```

## TODO
- [ ] Написати `docker-compose.yml` і `Dockerfile` сервера (multi-stage збирання).
- [x] Датасет у контейнері: `./data/docs` монтується в `/data/docs` (read-only), шлях задається змінною `DATA_DIR`.
