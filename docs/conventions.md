# Conventions

## Git
- Репозиторій на GitHub, назва: `course_work_parallel_computing`.
- Git обов'язковий; без історії комітів роботу не приймають.
- Коміти атомарні (одна змістовна зміна) і за [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <короткий опис в наказовому способі>
```

Типи: `feat`, `fix`, `docs`, `test`, `refactor`, `perf`, `build`, `ci`, `chore`.
Приклади:
```
feat(index): add sharded inverted index
feat(pool): implement fixed-size thread pool
build(docker): add compose with server and ollama services
docs(uml): add sequence diagram for search
```
- Не комітити: датасет, зібрані артефакти, моделі Ollama (додати в `.gitignore`).
- Коміти робити поступово протягом семестру, а не одним великим перед здачею.

## Код
- Один стиль форматування на весь проєкт ([Google Style Guides](https://google.github.io/styleguide/) для обраної мови), перевірка автоформатером.
- Принципи «Чистого коду» Р. Мартіна: осмислені імена, короткі функції, без дублювання, без «магічних» чисел.
- Коментарі пояснюють «чому», а не «що»; обов'язково для складних місць синхронізації.
- Конфігурація (порти, URL Ollama, модель, шлях до датасету) — через змінні середовища.

## Документація
- `README.md` у корені: опис, вимоги, збирання, запуск (`docker compose up`), приклади запитів, запуск тестів.
- Зміна контракту API — спочатку в `docs/api.md`.

## Мова
- Код, коментарі, коміти — англійською.
- Звіт і README — українською.
