#!/usr/bin/env python3
"""Build the Ukrainian coursework PDF from real measurements and verification.

Dependencies: reportlab; pypdfium2 is optional for PNG chart exports.
Run from any directory: python tools/build_report.py --author 'Name' --group 'Group'.
"""
import argparse
import json
from pathlib import Path
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (BaseDocTemplate, Frame, Image, KeepTogether,
                               NextPageTemplate, PageBreak, PageTemplate,
                               Paragraph, Preformatted, Spacer, Table, TableStyle)
from reportlab.platypus.tableofcontents import TableOfContents

from plot_results import ROOT, NAVY, build

OUTPUT = ROOT / "output/pdf/coursework_report.pdf"


class Report(BaseDocTemplate):
    def beforeDocument(self):
        self.bookmark_count = 0

    def afterFlowable(self, flowable):
        if isinstance(flowable, Paragraph) and flowable.style.name in {"Heading1", "Heading2"}:
            level = 0 if flowable.style.name == "Heading1" else 1
            key = f"section-{self.bookmark_count}"
            self.bookmark_count += 1
            self.canv.bookmarkPage(key)
            self.notify("TOCEntry", (level, flowable.getPlainText(), self.page, key))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author", default="Andrii Voloshyn")
    parser.add_argument("--group", default="")
    args = parser.parse_args()
    summary, drawings = build()
    results = ROOT / "bench/results"
    environment = json.loads((results / "environment.json").read_text(encoding="utf-8"))
    verification = json.loads((results / "verification.json").read_text(encoding="utf-8"))
    cases = json.loads((results / "search_cases.json").read_text(encoding="utf-8"))
    e1, e2 = summary["index"], summary["throughput"]
    best_index = min(e1, key=lambda item: item["median"])
    best_load = max(e2, key=lambda item: item["median"])
    dataset = environment["dataset"]
    request_count = int(environment["options"]["requests"])
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    styles = getSampleStyleSheet()
    for name in ["Normal", "BodyText", "Heading1", "Heading2", "Title"]:
        styles[name].fontName = "Report-Bold" if name in {"Heading1", "Heading2", "Title"} else "Report"
        styles[name].textColor = colors.HexColor("#18232e")
    styles["Normal"].fontSize = 10.5
    styles["Normal"].leading = 14
    styles["Normal"].spaceAfter = 6
    styles["Normal"].alignment = TA_JUSTIFY
    styles["Normal"].allowWidows = 0
    styles["Normal"].allowOrphans = 0
    styles["Heading1"].keepWithNext = True
    styles["Heading2"].keepWithNext = True
    styles["Heading1"].fontSize, styles["Heading1"].leading = 17, 23
    styles["Heading1"].spaceAfter = 16
    styles["Heading2"].fontSize, styles["Heading2"].leading = 12.5, 18
    styles["Heading2"].spaceBefore, styles["Heading2"].spaceAfter = 10, 6
    styles["Title"].fontSize, styles["Title"].leading = 20, 28
    styles.add(ParagraphStyle("Caption", fontName="Report", fontSize=9, leading=12,
                             alignment=TA_CENTER, spaceBefore=8, spaceAfter=10))
    styles.add(ParagraphStyle("Cell", fontName="Report", fontSize=9, leading=12))
    styles.add(ParagraphStyle("CellHead", parent=styles["Cell"], fontName="Report-Bold",
                             textColor=colors.white))
    styles.add(ParagraphStyle("Cover", fontName="Report", fontSize=12, leading=18,
                             alignment=TA_CENTER, spaceAfter=10))
    styles.add(ParagraphStyle("CodeBlock", fontName="Courier", fontSize=8.2,
                             leading=11, backColor=colors.HexColor("#f0f3f6"),
                             borderPadding=8, spaceBefore=8, spaceAfter=12))

    doc = Report(str(OUTPUT), pagesize=A4, leftMargin=25 * mm, rightMargin=20 * mm,
                 topMargin=20 * mm, bottomMargin=20 * mm,
                 title="Пошукова система з паралельними обчисленнями та LLM",
                 author=args.author)

    def decorate(canvas, document):
        if document.page == 1:
            return
        width, height = canvas._pagesize
        canvas.saveState()
        canvas.setFont("Report", 8)
        canvas.setFillColor(colors.HexColor("#526373"))
        canvas.drawString(25 * mm, height - 12 * mm, "Паралельні обчислення | Пошукова система з LLM")
        canvas.drawRightString(width - 20 * mm, 12 * mm, str(document.page))
        canvas.restoreState()

    portrait = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height,
                     leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
    wide_size = landscape(A4)
    wide = Frame(20 * mm, 18 * mm, wide_size[0] - 40 * mm, wide_size[1] - 38 * mm,
                 leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
    doc.addPageTemplates([PageTemplate(id="Portrait", frames=[portrait], pagesize=A4, onPage=decorate),
                          PageTemplate(id="Landscape", frames=[wide], pagesize=wide_size, onPage=decorate)])
    story = []

    def p(text, style="Normal"):
        return Paragraph(escape(str(text)), styles[style])

    def paragraph(text):
        story.append(p(text))

    def heading(text, level=1):
        story.append(p(text, f"Heading{level}"))

    def table(headers, rows, widths=None):
        content = [[p(text, "CellHead") for text in headers]]
        content += [[p(text, "Cell") for text in row] for row in rows]
        value = Table(content, colWidths=widths, repeatRows=1, hAlign="LEFT")
        value.setStyle(TableStyle([
            ("BACKGROUND", (0, 0), (-1, 0), NAVY),
            ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ("GRID", (0, 0), (-1, -1), .35, colors.HexColor("#cbd4dd")),
            ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f2f5f7")]),
            ("LEFTPADDING", (0, 0), (-1, -1), 6),
            ("RIGHTPADDING", (0, 0), (-1, -1), 6),
            ("TOPPADDING", (0, 0), (-1, -1), 7),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 7),
        ]))
        story.extend([value, Spacer(1, 10)])

    def chart(name, caption):
        drawing = drawings[name]
        factor = doc.width / drawing.width
        drawing.scale(factor, factor)
        drawing.width *= factor
        drawing.height *= factor
        story.append(KeepTogether([drawing, p(caption, "Caption")]))

    def diagram(filename, title, caption, wide_page=False, new_page=True):
        if new_page:
            story.extend([NextPageTemplate("Landscape" if wide_page else "Portrait"), PageBreak()])
        heading(title, 2)
        image = Image(str(ROOT / "docs/diagrams" / filename))
        image._restrictSize(wide_size[0] - 40 * mm if wide_page else doc.width,
                            385 if wide_page else 560)
        story.append(KeepTogether([image, p(caption, "Caption")]))

    story += [Spacer(1, 18 * mm), p("КПІ ім. Ігоря Сікорського", "Cover"),
              p("Навчально-науковий інститут прикладного системного аналізу", "Cover"),
              p("Кафедра системного проєктування", "Cover"), Spacer(1, 18 * mm),
              p("КУРСОВА РОБОТА", "Title"), p("з дисципліни «Паралельні обчислення»", "Cover"),
              Spacer(1, 10 * mm), p("Дослідження та реалізація паралельних обчислень на прикладі побудови пошукової системи з інтеграцією LLM", "Title"),
              Spacer(1, 15 * mm), p("Виконавець: " + args.author, "Cover")]
    if args.group:
        story.append(p("Група: " + args.group, "Cover"))
    story += [p("Керівник: Яременко В. С.", "Cover"),
              p("Пояснювальна записка та результати експериментів", "Cover"),
              Spacer(1, 16 * mm), p("Київ - 2026", "Cover"), PageBreak()]
    story.append(p("ЗМІСТ", "Title"))
    toc = TableOfContents()
    toc.levelStyles = [ParagraphStyle("TOC0", fontName="Report-Bold", fontSize=11,
                                     leading=17, spaceBefore=8),
                       ParagraphStyle("TOC1", fontName="Report", fontSize=9.5,
                                     leading=14, leftIndent=12)]
    story += [toc, PageBreak()]

    heading("1. Вступ")
    paragraph("Мета роботи - реалізувати й дослідити контейнеризовану пошукову систему, яка демонструє паралельну побудову інвертованого індексу, потокобезпечний доступ до нього та одночасну обробку клієнтських запитів. Локальна мовна модель розширює запит синонімами та спорідненими термінами перед keyword-пошуком. Результати доступні через REST API, консольний клієнт і вебінтерфейс з WebSocket.")
    heading("1.1. Постановка задачі та критерії", 2)
    paragraph("Система має запускатися командою docker compose up; містити пошуковий сервер та Ollama; реалізовувати власні thread pool і паралельний інвертований індекс; підтримувати параметр threads. Обов'язкові маршрути: /api/health, /api/index, /api/search, /api/stats і /ws/search. Додатково потрібні два клієнти, UML, графіки масштабованості та пояснювальна записка [1].")
    paragraph("Вхідні дані - англомовні текстові документи. У силабусі одночасно наведено межі 1 ГБ та 2 ГБ. Обрано суворіший варіант: щонайменше 2 GiB і 10 000 документів. Самостійна генерація дозволена; типовий full-профіль створює 12 000 документів. Підготовка текстів не залежить від мережі. Для початкового отримання Docker-образів, бібліотек і моделі мережа потрібна.")
    heading("1.2. Теоретичні положення", 2)
    paragraph("Інвертований індекс відображає термін у список документів, де він зустрічається. Запис postings містить ідентифікатор документа і частоту терміна. Індекс зменшує обсяг роботи під час запиту: замість повторного читання всього корпусу пошук обробляє postings потрібних слів. У роботі окремо вимірюються побудова індексу та його використання.")
    paragraph("Потік має власний стек і виконує послідовність інструкцій у спільному адресному просторі процесу. Пул зберігає фіксований набір потоків та чергу завдань; це дозволяє уникнути створення потоку для кожного файла. Mutex захищає спільний стан, а condition_variable дозволяє працівникам чекати появи роботи без активного циклу. Очікування використовує предикат, оскільки пробудження не гарантує виконання умови [2, 3].")
    paragraph("Прискорення визначено як S(p)=T(1)/T(p), ефективність - E(p)=S(p)/p. Ідеальне лінійне прискорення обмежують послідовна частина, синхронізація, доступ до пам'яті, дисковий ввід-вивід і планування. Модель Амдала S(p)=1/(s+(1-s)/p) пояснює, чому додаткові потоки не гарантують пропорційного виграшу. Параметр s у реальній системі не можна точно встановити лише з одного вимірювання прискорення.")
    heading("1.3. Обрані технології", 2)
    paragraph("C++20 і CMake обрано для компільованого backend та контролю багатопотоковості. Crow 1.3.4 надає HTTP/WebSocket; cpp-httplib 0.65.0 виконує HTTP-виклики; nlohmann/json 3.11.3 обробляє JSON. Клієнти реалізовані на C++ та Vue 3.5.13. Локальна Ollama 0.40.10 використовує qwen2.5:1.5b. Корпус і навантаження створюються Python stdlib; перевірки - GoogleTest 1.15.2, Node, Python unittest і ThreadSanitizer.")

    story.append(PageBreak())
    heading("2. Модель системи")
    paragraph("Модель має шари транспорту, сервісів та ядра. API перевіряє запит і делегує роботу IndexService або SearchPipeline. Власний ThreadPool виконує файлові завдання побудови і фонові завдання WebSocket. Зовнішні бібліотеки не замінюють ці реалізації. UML відповідає фактичним класам, маршрутам та Docker Compose.")
    paragraph("Виділено чотири основні сценарії: UC1 - побудова/перебудова індексу; UC2 - REST-пошук з CLI; UC3 - потоковий пошук у вебінтерфейсі; UC4 - читання стану та статистики. Для кожного наведено окрему sequence-діаграму. Автоматичний старт використовує той самий маршрут побудови індексу.")
    diagram("use-case.png", "2.1. Варіанти використання", "Рисунок 2.1 - Актори, сценарії та участь Ollama.", new_page=False)
    diagram("class.png", "2.2. Класи ядра", "Рисунок 2.2 - IndexService, власний ThreadPool та шардований InvertedIndex.")
    diagram("class-services.png", "2.3. Класи сервісного шару", "Рисунок 2.3 - Пайплайн, транспортні обробники та інтеграція Ollama.")
    diagram("sequence-index.png", "2.4. UC1: побудова індексу", "Рисунок 2.4 - Паралельна побудова та атомарна публікація покоління.", True)
    diagram("sequence-rest.png", "2.5. UC2: REST-пошук", "Рисунок 2.5 - CLI, розширення запиту, fallback і keyword-пошук.", True)
    diagram("sequence-websocket.png", "2.6. UC3: потоковий пошук", "Рисунок 2.6 - Окремі події етапів без блокування I/O-обробника.", True)
    diagram("sequence-health.png", "2.7. UC4: стан та статистика", "Рисунок 2.7 - Кешований health, узгоджені stats і додаткова готовність.", True)
    diagram("deployment.png", "2.8. Розгортання Docker Compose", "Рисунок 2.8 - Три сервіси, HTTP/WebSocket-зв'язки та постійні томи.", True)

    story.extend([NextPageTemplate("Portrait"), PageBreak()])
    heading("3. Опис реалізації")
    heading("3.1. Власний пул потоків", 2)
    paragraph("ThreadPool реалізовано в src/core/thread_pool.h і .cpp на std::thread, std::deque, std::mutex та std::condition_variable. Submit додає задачу під блокуванням і сповіщає одного працівника. WorkerLoop витягує задачу під mutex, але виконує її поза критичною секцією. Тому довгий файловий ввід чи пошук не блокує додавання інших задач.")
    paragraph("Shutdown припиняє приймання завдань, пробуджує працівників, дозволяє завершити чергу та виконує join. Окремий mutex серіалізує конкурентні виклики Shutdown. Якщо конструктор не зміг створити всі потоки, вже створені потоки зупиняються та приєднуються перед повторним викиданням винятку. Виняток окремого завдання не завершує працівника. Виклик Shutdown із задачі цього ж пулу не підтримується.")
    paragraph("Для індексації пул створюється на один виклик Build(p). Для WebSocket він є полем SearchSocketService. Готові ExecutorService, thread_pool сторонніх бібліотек або бібліотечний інвертований індекс не використовуються. I/O-потоки Crow належать транспортному рівню і не замінюють навчальний пул.")
    heading("3.2. Індекс та безпечна публікація", 2)
    paragraph("InvertedIndex містить 32 шарди. У кожному зберігається unordered_map від нормалізованого терміна до vector<Posting> та окремий shared_mutex. Частоти слів одного документа спочатку обчислюються локально. Потім терміни групуються за shard; кожен потрібний shard блокується один раз на документ. Операція не тримає одночасно mutex двох шардів. Метадані документів мають окремий mutex, лічильник postings - atomic.")
    paragraph("IndexService читає лише звичайні .txt файли, сортує їх і передає завдання власному пулу. Помилки читання збираються і спричиняють відмову всієї нової побудови. Після завершення працівників новий shared_ptr<const InvertedIndex> публікується разом з BuildStats під state_mutex. Читач отримує узгоджений snapshot. Помилка перебудови зберігає попередній індекс. Одночасна друга побудова повертає HTTP 409; нові запити пошуку під час побудови також отримують 409.")
    heading("3.3. Keyword-пошук і ранжування", 2)
    paragraph("Той самий tokenizer використовується для документів, запитів і термінів від моделі. Дублікати термінів запиту видаляються. Для кожного postings використовується нормалізована частота tf(t,d)=count(t,d)/length(d) і згладжена вага idf(t)=ln(1+N/df(t)). Оцінка документа є сумою tf·idf за термінами запиту. Результати нормалізуються до найкращого score=1. Це конкретна реалізація з TF-IDF вагами, а не косинусна подібність.")
    paragraph("Система повертає до 10 результатів. За однакового score порядок визначається зовнішнім doc_id. Snippet читається з файла та містить контекст знайденого токена; пробіли нормалізуються. Якщо термінів немає в індексі, повертається успішний порожній results. Некоректний запит або недоступний індекс мають окремий статус помилки.")
    heading("3.4. LLM, REST і WebSocket", 2)
    paragraph("OllamaClient надсилає POST /api/generate з назвою моделі, інструкцією повернути синоніми, stream=false та temperature=0 [4]. Відповідь очищається від маркерів списку, токенізується та обмежується вісьмома новими унікальними термінами. Числові маркери не витісняють корисні слова. Повна генерація має deadline 5 с; health-проба - 500 мс. Помилка або timeout дає порожнє розширення й пошук за оригінальним запитом.")
    paragraph("SearchPipeline викликає observer.on_expansion до запуску SearchEngine, тому подія query_expansion передається клієнту до завершення keyword-пошуку. Далі надсилаються keyword_search та done. SearchSocketService використовує власний пул з 4 працівників за замовчуванням, глобальну чергу на 64 очікувальні завдання та ліміт 4 на з'єднання. Запити одного з'єднання обслуговуються FIFO. Закриття сесії забороняє подальші відправлення.")
    table(["Маршрут", "Запит / успішна відповідь"], [
        ["GET /api/health", '{"status":"ok","ollama":true|false}'],
        ["POST /api/index", '{"threads":4} → {"status":"ok","documents_indexed":12000,"time_ms":...}'],
        ["POST /api/search", '{"query":"coral reef","mode":"keyword"} → {"results":[{"doc_id":"...","score":1,"snippet":"..."}],"time_ms":...}'],
        ["GET /api/stats", '{"documents":12000,"index_build_time_ms":...,"threads_used":4}'],
        ["WS /ws/search", "{query} → query_expansion → keyword_search → done"],
    ], [115, doc.width - 115])
    heading("3.5. Розгортання, корпус та клієнти", 2)
    paragraph("Типовий docker compose up запускає server, ollama та одноразовий model-init. Останній чекає API Ollama, перевіряє наявність вибраної моделі, за потреби завантажує її й виконує прогрів з keep_alive=24h. Модель зберігається в ollama_models. Compose задає залежність від запуску Ollama, але не затримує появу вебінтерфейсу до завершення завантаження моделі [5].")
    paragraph("Python supervisor працює як PID 1 контейнера server. Він одразу запускає C++ HTTP-сервер, паралельно готує корпус, потім викликає POST /api/index. Стан записується атомарною заміною невеликого JSON-файла; /api/ready читає його та повідомляє помилку підготовки. /api/health зберігає фіксований контракт з двома полями. Supervisor передає сигнали завершення дочірнім процесам і очікує їх. Генерація не виконується через shell-команду з підставленими змінними.")
    paragraph(f"Корпус full відтворюється з seed={dataset['seed']}. Фактичний обсяг виміряного корпусу - {dataset['bytes']:,} байтів; документів - {dataset['documents']:,}. Генератор використовує Python stdlib, різні теми й частоти англійських слів. Staging-каталог перейменовується лише після успіху; чужі непорожні дані не перезаписуються. Manifest містить версію, seed, розмір і SHA-256. Повторний старт перевіряє і використовує готові файли. Це синтетичний текст, не колекція справжніх книг; висновки про семантичну якість натуральних корпусів потребують окремого дослідження.")
    paragraph("C++ CLI search викликає REST, друкує doc_id, score і snippet та підтримує --server, SEARCH_URL і --json. Коди завершення: 0 - успішний запит, зокрема без збігів; 1 - помилка мережі, сервера або відповіді; 2 - неправильні аргументи. Vue отримує події WebSocket, показує етапи та готовність; текст результатів не вставляється як HTML. Vue й ресурси сторінки локальні та вбудовані у C++ бінарник.")
    story.append(Preformatted('docker compose up --build -d\ndocker compose exec server search "coral reef"\ndocker compose exec server search --json "quantum physics"\n# Standalone build: ./build/search "coral reef"', styles["CodeBlock"]))

    story.append(PageBreak())
    heading("4. Тестування та результати")
    heading("4.1. Перевірка функціональності й конкурентності", 2)
    paragraph(f"Перевірка виконана {verification['date']}. Повний набір CTest: {verification['ctest_passed']}/{verification['ctest_total']} успішних перевірок. Окремі поведінкові suites містять {verification['ui_tests']} Node-тестів інтерфейсу, 7 тестів генератора, 6 тестів CLI та 3 тести підготовки моделі. Для клієнтів використано локальні HTTP-fixtures: правильна відповідь, порожній результат, неправильний JSON, HTTP 409, недоступність сервера, перше завантаження моделі, повторне використання та помилка завантаження.")
    paragraph(f"ThreadSanitizer: {verification['tsan_passed']}/{verification['tsan_total']} перевірок на актуальних вихідних текстах, без runtime-повідомлень про гонки. Застосовано ENABLE_TSAN=ON, TSAN_OPTIONS=halt_on_error=1 та setarch x86_64 -R в окремому тестовому контейнері. GCC попереджає про обмежену підтримку atomic_thread_fence у сторонній Asio. TSan знаходить динамічні гонки у виконаних сценаріях; успішний прогін не доводить відсутність усіх можливих гонок [6].")
    paragraph("Тести власного пулу перевіряють паралельне виконання, спорожнення черги, конкурентне завершення та відмову створення потоку. Тести індексу й IndexService перевіряють багатопотоковий запис, точність статистики, конфлікт перебудови, помилки файлів і збереження попереднього snapshot. Мережеві перевірки контролюють JSON-контракт, порядок етапів WebSocket та поведінку при закритті сесії.")
    table(["Контрольний сценарій", "Фактичний результат"], [
        ["Стандартний Compose", verification["compose"]],
        ["Холодний HTTP / готовність пошуку", verification["cold_start"]],
        ["Справжня Ollama", verification["ollama"]],
        ["WebSocket", verification["websocket"]],
        ["CLI", verification["cli"]],
        ["Браузер", verification["browser"]],
    ], [145, doc.width - 145])
    heading("4.2. Умови та методика експериментів", 2)
    disk = environment.get("disk") or {}
    disk_text = f"{disk.get('FriendlyName', 'не зафіксовано')}, {disk.get('BusType', '')}"
    paragraph(f"Вимірювання виконано {environment['started_utc'][:10]}: {environment['cpu_model']}; Docker бачить {environment['docker_cpus']} логічних CPU і {environment['docker_memory_bytes'] / 1024**3:.2f} GiB RAM. Docker Engine {environment['docker_version']}; Linux/WSL2; клієнт {environment['client_platform']}, Python {environment['python']}. Окремих обмежень Memory/NanoCpus/Cpuset контейнеру не задано. Дані розташовані в Linux named volume на віртуальному диску WSL2. Накопичувач хоста: {disk_text}.")
    paragraph(f"Для кожної точки виконано один неврахований прогрів і п'ять виміряних запусків. E1 використовує 1, 2, 4, 8, 12 і 16 потоків та той самий full-корпус. Час береться з time_ms сервера; окремо збережено клієнтський wall_ms. Індекс перебудовується повністю. Це дослід з прогрітим файловим кешем; очищення кешу ОС не виконувалося. E2 має 1, 2, 4, 8, 16, 32 та 64 клієнти, по {request_count} HTTP-запитів у кожному повторенні, з новим HTTP-з'єднанням на запит.")
    paragraph("Для E2 створено окремий сервер QUERY_EXPANSION=false. Так вимірюється саме keyword-пошук без домінування часу генерації LLM. У типовому Compose QUERY_EXPANSION=true. Час time_ms пошуку відображає keyword-етап; він не є повною затримкою REST-пайплайну. Для навантаження використано фактичний wall time, p50 і p95 повних HTTP-запитів. Raw CSV, environment.json, image digest і набір запитів збережено в bench/results. Порівнювати ці числа з результатами іншого корпусу або обладнання без повторення методики не можна.")
    heading("4.3. Паралельна побудова індексу", 2)
    table(["Потоки", "Медіана, с", "Min-max, с", "S(p)", "E(p), %"], [
        [r["threads"], f"{r['median']/1000:.3f}", f"{r['min']/1000:.3f} - {r['max']/1000:.3f}",
         f"{r['speedup']:.2f}", f"{r['efficiency']*100:.1f}"] for r in e1
    ], [55, 90, 145, 65, doc.width - 355])
    chart("time", "Рисунок 4.1 - Повна побудова: медіани та діапазони п'яти запусків.")
    chart("speedup", "Рисунок 4.2 - Фактичне та ідеальне прискорення.")
    paragraph(f"Медіана часу зменшилася з {e1[0]['median']/1000:.3f} с при одному потоці до {best_index['median']/1000:.3f} с при {best_index['threads']} потоках. Максимальне виміряне прискорення становить {best_index['speedup']:.2f}, ефективність - {best_index['efficiency']*100:.1f}%. У межах досліду критерій зменшення часу при збільшенні кількості потоків підтверджено. Найкраща точка цього досліду не є універсальним налаштуванням для кожного комп'ютера.")
    heading("4.4. Паралельні клієнти", 2)
    table(["Клієнти", "Медіана, зап./с", "p50, мс", "p95, мс", "Помилки"], [
        [r["clients"], f"{r['median']:.1f}", f"{r['p50_ms']:.1f}", f"{r['p95_ms']:.1f}", r["errors"]]
        for r in e2
    ], [60, 130, 85, 85, doc.width - 360])
    chart("throughput", "Рисунок 4.3 - Пропускна здатність; вісь клієнтів має шкалу log2.")
    chart("latency", "Рисунок 4.4 - Медіана p95 п'яти повторень, а не p95 усіх об'єднаних запитів.")
    paragraph(f"Найбільша медіанна пропускна здатність - {best_load['median']:.1f} запитів/с при {best_load['clients']} клієнтах, порівняно з {e2[0]['median']:.1f} при одному. При переході до 32-64 клієнтів throughput не дає пропорційного виграшу, а хвостова затримка збільшується. У {sum(r['runs'] for r in e2)*request_count} виміряних запитах зафіксовано {sum(r['errors'] for r in e2)} помилок. Отже, конкурентність корисна до насичення ресурсів, після чого більша кількість клієнтів переважно збільшує очікування.")
    heading("4.5. Аналіз вузьких місць та обмежень", 2)
    paragraph("Побудова має послідовні частини: перелік і сортування файлів, диспетчеризація, публікація нового покоління та звільнення попереднього. Під час додавання документів потоки конкурують за metadata mutex і за популярні шарди. Крім блокувань, впливають пропускна здатність пам'яті, алокації та читання файлів. Результат S(p)<p узгоджується з цими обмеженнями. Це пояснення за структурою коду та графіками; окремий profiler не встановлював точну частку кожного чинника.")
    paragraph("У keyword-пошуку кожен postings потребує читання довжини документа під спільним metadata mutex. Повні списки postings копіюються, а всі кандидати сортуються перед вибором top-10. Після насичення CPU/пам'яті це підсилює конкуренцію. Поліпшеннями можуть бути незмінні масиви метаданих у published snapshot, вибір top-K замість повного сортування та вимірювання очікування блокувань. Їх ефект потрібно перевіряти новими дослідами, а не вважати доведеним.")
    paragraph("Навантаження генерувалося з Windows на той самий фізичний комп'ютер, тому частина вартості припадає на клієнт, нові TCP-з'єднання й мережевий шар Docker Desktop. CPU-частота, температура та стороння активність ОС не фіксувалися. Діапазони min-max показують варіативність; особливо важливо не робити висновки з одного найшвидшого запуску. Синтетичний словник і частоти не відтворюють природні книги. E2 не описує пропускну здатність мовної моделі; для неї потрібен окремий end-to-end експеримент.")
    heading("4.6. Тестові пошукові запити", 2)
    table(["Запит", "Очікувано", "Фактично", "Перевірка"], [
        [r["query"], "Є результати" if r["expected"] == "nonempty" else "Немає збігів",
         f"{r['actual_count']} результатів", "Пройдено" if r["passed"] else "Помилка"] for r in cases
    ], [135, 110, 110, doc.width - 355])
    paragraph("Очікування сформовані за контрольними фразами manifest та словом, відсутнім у генераторі. Таблиця перевіряє факт знаходження відомих термінів, а не precision/recall на незалежній розміченій колекції. Повні фактичні doc_id, score та snippet збережено в search_cases.json. Якість query expansion додатково перевірено реальною Ollama та порядком подій WebSocket.")

    story.append(PageBreak())
    heading("5. Висновки")
    paragraph("Реалізовано C++20 пошукову систему з власними пулом потоків та шардованим інвертованим індексом. Паралельна побудова параметризується кількістю працівників. Узгоджені snapshots, локальні критичні секції та завершення працівників дозволяють керувати спільним станом. REST і WebSocket дотримуються контракту завдання; вебінтерфейс та C++ CLI забезпечують два способи використання.")
    paragraph(f"Дослідження на {dataset['documents']:,} документах обсягом понад 2 GiB продемонструвало прискорення побудови {best_index['speedup']:.2f} разів у найкращій виміряній точці. Максимальна медіана keyword-throughput становила {best_load['median']:.1f} запитів/с; зі збільшенням числа клієнтів до 32-64 приріст був значно меншим за пропорційний, а затримка зростала. Отримані результати показують користь паралелізму та його межі, а не необмежене лінійне масштабування.")
    paragraph("Стандартний Compose автоматизує підготовку корпусу, індексацію, запуск Ollama та підготовку моделі. Вебсторінка доступна до завершення підготовки, проте пошук по повному корпусу природно потребує часу на створення даних та індексу. Початкове завантаження моделі також потребує мережі. Подальший розвиток доцільно спрямувати на зменшення конкуренції за метадані, частковий top-K та дослідження натуральних корпусів і повного LLM-пайплайну.")
    heading("Перелік джерел")
    sources = [
        "1. Яременко В. С. Паралельні обчислення. Курсова робота: силабус 2026-2027. Додатки А, Б; критерії та контракт, с. 5-10. Наданий документ завдання.",
        "2. C++ working draft. Mutex requirements. https://eel.is/c++draft/thread.mutex.requirements (дата звернення: 23.11.2026).",
        "3. C++ working draft. Condition variables. https://eel.is/c++draft/thread.condition (дата звернення: 23.11.2026).",
        "4. Ollama documentation. Generate a response; Pull a model. https://docs.ollama.com/api/generate ; https://docs.ollama.com/api/pull (дата звернення: 23.11.2026).",
        "5. Docker documentation. Define services in Docker Compose. https://docs.docker.com/reference/compose-file/services/ (дата звернення: 23.11.2026).",
        "6. GCC documentation. Instrumentation Options, -fsanitize=thread. https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html (дата звернення: 23.11.2026).",
        "7. Репозиторій вихідного коду та відтворювані результати: https://github.com/voloshyyn/parallel-coursework . Скрипти bench/run.py, tools/plot_results.py і tools/build_report.py.",
        "8. Manning C. D., Raghavan P., Schutze H. Introduction to Information Retrieval. Cambridge University Press, 2008. Рекомендована силабусом література.",
    ]
    for source in sources:
        paragraph(source)
    story.append(Paragraph('<link href="https://github.com/voloshyyn/parallel-coursework" color="#183d62">Відкрити репозиторій GitHub</link>', styles["Normal"]))
    paragraph("Відтворення: команди в README.md та bench/README.md; редаговані UML - docs/diagrams/*.puml; CSV і протокол - bench/results; графіки - bench/plots. Для регенерації PDF виконати tools/build_report.py (reportlab, Arial або DejaVu Sans). Корпуси, моделі й бінарники зберігаються поза Git.")
    doc.multiBuild(story)
    print(OUTPUT)


if __name__ == "__main__":
    main()
