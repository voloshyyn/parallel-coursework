#!/usr/bin/env python3
"""Build the Ukrainian coursework report (PDF) from real code, results and logs.

Typography follows the usual Ukrainian coursework rules: A4, Times New Roman
14 pt, line spacing 1.5, first-line indent 1.25 cm, margins left 30 mm, right
10 mm, top and bottom 20 mm, page numbers at the top centre.

Every number comes from the committed measurements (bench/results), the
verification protocol, the CTest log or the source tree; code listings are cut
out of the real source files at build time, so the report cannot drift from the
code. Dependencies: reportlab (pypdfium2 is optional for PNG chart exports).

Run from any directory:
    python tools/build_report.py --author "Прізвище Ім'я" --group "ІПС-41"
"""
import argparse
import collections
import json
import re
from pathlib import Path
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_JUSTIFY, TA_LEFT
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.platypus import (BaseDocTemplate, Frame, Image, KeepTogether,
                               NextPageTemplate, PageBreak, PageTemplate,
                               Paragraph, Preformatted, Spacer, Table, TableStyle)
from reportlab.platypus.tableofcontents import TableOfContents

from plot_results import ROOT, build

OUTPUT = ROOT / "output/pdf/coursework_report.pdf"
INDENT = 35.4  # 1.25 cm first-line indent
GRID = colors.black
HEAD_FILL = colors.HexColor("#e6e6e6")
CODE_FILL = colors.HexColor("#f3f3f3")

SUITE_NOTES = {
    "ThreadPool": "Власний пул потоків: виконання задач, завершення, винятки, конкурентні виробники та Shutdown",
    "Tokenizer": "Правила токенізації та нормалізації (ASCII, кирилиця, пунктуація, некоректний UTF-8)",
    "InvertedIndex": "Шардований інвертований індекс: паралельний запис, Lookup, лічильники, помилки",
    "IndexService": "Паралельна побудова, публікація покоління, конфлікт 409, помилки читання файлів",
    "SearchEngine": "TF-IDF ранжування, порядок результатів, фрагменти (snippets)",
    "SearchPipeline": "Розширення запиту, запасний варіант, події етапів",
    "OllamaClient": "Клієнт Ollama: формування запиту, розбір відповіді, помилки та обмеження часу",
    "OllamaMonitor": "Фонова перевірка доступності Ollama",
    "OllamaModelHealth": "Доступність саме вибраної моделі",
    "HealthEndpoint": "Маршрут /api/health: кешована відповідь без очікування мережі",
    "SearchApi": "REST-контракт: коди відповідей, перевірка параметра threads, форма JSON",
    "SearchSocket": "Обробка WebSocket-повідомлень: черги, порядок FIFO, переповнення, закриття",
    "SearchSocketRoute": "Справжній WebSocket-маршрут Crow: порядок етапів і помилки",
    "WebUi": "Вбудовані статичні ресурси вебінтерфейсу",
    "TempDir": "Допоміжні тимчасові каталоги тестів",
    "ui_search_client": "Клієнтська логіка вебінтерфейсу (Node)",
    "model_preparation": "Підготовка моделі Ollama (Python)",
    "dataset_generator": "Генератор синтетичного корпусу (Python)",
    "cli_client": "Консольний клієнт search (Python-сценарії з HTTP-стендом)",
}

class Report(BaseDocTemplate):
    def beforeDocument(self):
        self.bookmark_count = 0

    def afterFlowable(self, flowable):
        if isinstance(flowable, Paragraph) and flowable.style.name in {"H1", "H2"}:
            level = 0 if flowable.style.name == "H1" else 1
            key = f"section-{self.bookmark_count}"
            self.bookmark_count += 1
            self.canv.bookmarkPage(key)
            self.notify("TOCEntry", (level, flowable.getPlainText(), self.page, key))


def num(value):
    """Format an integer with non-breaking spaces as thousands separators."""
    return f"{value:,}".replace(",", " ")


def comma(text):
    """Replace decimal points between digits with commas (no version strings here)."""
    return re.sub(r"(?<=\d)\.(?=\d)", ",", text)


def fx(value, digits):
    """Format a float with a decimal comma, as Ukrainian typography requires."""
    return f"{value:.{digits}f}".replace(".", ",")


def rich(text):
    """Escape text but keep a tiny whitelist of inline tags (<b>, <i>)."""
    safe = escape(str(text))
    for tag in ("b", "i"):
        safe = safe.replace(f"&lt;{tag}&gt;", f"<{tag}>").replace(f"&lt;/{tag}&gt;", f"</{tag}>")
    return safe


def excerpt(relative, signature):
    """Return a top-level function body, cut out of the real source file."""
    lines = (ROOT / relative).read_text(encoding="utf-8").splitlines()
    for start, line in enumerate(lines):
        if signature in line and not line.startswith(" "):
            for end in range(start, len(lines)):
                if lines[end] == "}":
                    return "\n".join(lines[start:end + 1])
    raise RuntimeError(f"{relative}: function {signature!r} not found")


def count_lines(patterns):
    total = 0
    for pattern in patterns:
        for path in ROOT.glob(pattern):
            if path.is_file():
                total += len(path.read_text(encoding="utf-8", errors="replace").splitlines())
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--author", default="Волошин Андрій")
    parser.add_argument("--group", default="")
    args = parser.parse_args()

    summary, drawings = build()
    results = ROOT / "bench/results"
    environment = json.loads((results / "environment.json").read_text(encoding="utf-8"))
    verification = json.loads((results / "verification.json").read_text(encoding="utf-8"))
    cases = json.loads((results / "search_cases.json").read_text(encoding="utf-8"))
    gutenberg = json.loads((results / "gutenberg_check.json").read_text(encoding="utf-8"))
    ctest_log = (results / "ctest.log").read_text(encoding="utf-8", errors="replace")
    suites = collections.Counter(name.split(".")[0] for name in re.findall(r"Test\s+#\d+:\s+(\S+)", ctest_log))
    e1, e2 = summary["index"], summary["throughput"]
    best_index = min(e1, key=lambda item: item["median"])
    best_load = max(e2, key=lambda item: item["median"])
    dataset = environment["dataset"]
    request_count = int(environment["options"]["requests"])
    loc = {
        "src": count_lines(["src/*.cpp", "src/*.h", "src/*/*.cpp", "src/*/*.h", "src/web/*.in"]),
        "tests": count_lines(["tests/*.cpp", "tests/*.h", "tests/*.py", "tests/ui/*.js"]),
        "web": count_lines(["web/app.js", "web/search_client.js", "web/style.css", "web/index.html"]),
        "tools": count_lines(["tools/*.py", "bench/run.py", "scripts/*.sh"]),
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)

    def style(name, font="Report", **kwargs):
        return ParagraphStyle(name, fontName=font, textColor=colors.black, **kwargs)

    styles = {
        "Body": style("Body", fontSize=14, leading=21, alignment=TA_JUSTIFY, firstLineIndent=INDENT,
                      allowWidows=0, allowOrphans=0),
        "Flat": style("Flat", fontSize=14, leading=21, alignment=TA_JUSTIFY, allowWidows=0, allowOrphans=0),
        "Source": style("Source", fontSize=14, leading=17, alignment=TA_LEFT, spaceAfter=2),
        "H1": style("H1", "Report-Bold", fontSize=14, leading=21, alignment=TA_CENTER, spaceAfter=21,
                    keepWithNext=True),
        "H2": style("H2", "Report-Bold", fontSize=14, leading=21, alignment=TA_LEFT, firstLineIndent=INDENT,
                    spaceBefore=14, spaceAfter=7, keepWithNext=True),
        "H3": style("H3", "Report-BoldItalic", fontSize=14, leading=21, alignment=TA_LEFT,
                    firstLineIndent=INDENT, spaceBefore=7, spaceAfter=3, keepWithNext=True),
        "Caption": style("Caption", fontSize=12, leading=15, alignment=TA_CENTER, spaceBefore=6, spaceAfter=14),
        "TableCaption": style("TableCaption", fontSize=12, leading=15, alignment=TA_LEFT, spaceBefore=10,
                              spaceAfter=4, keepWithNext=True),
        "TableCaptionLoose": style("TableCaptionLoose", fontSize=12, leading=15, alignment=TA_LEFT,
                                   spaceBefore=10, spaceAfter=4),
        "Cell": style("Cell", fontSize=12, leading=14, alignment=TA_LEFT),
        "CellHead": style("CellHead", "Report-Bold", fontSize=12, leading=14, alignment=TA_CENTER),
        "Code": style("Code", "Report-Mono", fontSize=8.5, leading=10.4, backColor=CODE_FILL,
                      leftIndent=4, borderPadding=4, spaceBefore=4, spaceAfter=12),
        "Bullet": style("Bullet", fontSize=14, leading=21, alignment=TA_JUSTIFY, leftIndent=INDENT + 17,
                        bulletIndent=INDENT, bulletFontName="Report", bulletFontSize=14,
                        firstLineIndent=0, allowWidows=0, allowOrphans=0),
        "Formula": style("Formula", fontSize=14, leading=21, alignment=TA_CENTER),
        "FormulaNo": style("FormulaNo", fontSize=14, leading=21, alignment=TA_LEFT),
        "Cover": style("Cover", fontSize=14, leading=20, alignment=TA_CENTER, spaceAfter=6),
        "CoverBold": style("CoverBold", "Report-Bold", fontSize=14, leading=20, alignment=TA_CENTER, spaceAfter=6),
        "CoverTitle": style("CoverTitle", "Report-Bold", fontSize=18, leading=26, alignment=TA_CENTER, spaceAfter=8),
        "CoverRight": style("CoverRight", fontSize=14, leading=20, alignment=TA_LEFT, leftIndent=250, spaceAfter=3),
    }

    doc = Report(str(OUTPUT), pagesize=A4, leftMargin=30 * mm, rightMargin=10 * mm,
                 topMargin=20 * mm, bottomMargin=20 * mm,
                 title="Дослідження та реалізація паралельних обчислень на прикладі побудови пошукової системи з інтеграцією LLM",
                 author=args.author, subject="Курсова робота з дисципліни «Паралельні обчислення»")

    def page_number(canvas, document):
        if document.page == 1:
            return
        width, height = canvas._pagesize
        canvas.saveState()
        canvas.setFont("Report", 12)
        canvas.drawCentredString(width / 2, height - 12 * mm, str(document.page))
        canvas.restoreState()

    portrait = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height,
                     leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
    wide_size = landscape(A4)
    wide = Frame(25 * mm, 15 * mm, wide_size[0] - 40 * mm, wide_size[1] - 33 * mm,
                 leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)
    doc.addPageTemplates([PageTemplate(id="Portrait", frames=[portrait], pagesize=A4, onPage=page_number),
                          PageTemplate(id="Landscape", frames=[wide], pagesize=wide_size, onPage=page_number)])
    story = []
    counters = collections.Counter()

    def p(text, name="Body"):
        return Paragraph(rich(text), styles[name])

    def para(text):
        story.append(p(text))

    def flat(text):
        story.append(p(text, "Flat"))

    def bullets(items):
        for item in items:
            story.append(Paragraph(rich(item), styles["Bullet"], bulletText="–"))

    def h1(text, new_page=True):
        if new_page:
            story.append(PageBreak())
        story.append(p(text.upper(), "H1"))

    def h2(text):
        story.append(p(text, "H2"))

    def h3(text):
        story.append(p(text, "H3"))

    def caption_table(section, title, keep=True):
        counters[section] += 1
        story.append(p(f"Таблиця {section}.{counters[section]} – {title}",
                       "TableCaption" if keep else "TableCaptionLoose"))

    def table(headers, rows, fractions, section=None, title=None, keep=True):
        if section:
            caption_table(section, title, keep)
        content = [[p(text, "CellHead") for text in headers]]
        for row in rows:
            content.append([p(text, "Cell") for text in row])
        widths = [fraction * doc.width for fraction in fractions]
        value = Table(content, colWidths=widths, repeatRows=1, hAlign="LEFT")
        value.setStyle(TableStyle([
            ("BACKGROUND", (0, 0), (-1, 0), HEAD_FILL),
            ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
            ("GRID", (0, 0), (-1, -1), 0.5, GRID),
            ("LEFTPADDING", (0, 0), (-1, -1), 4), ("RIGHTPADDING", (0, 0), (-1, -1), 4),
            ("TOPPADDING", (0, 0), (-1, -1), 3), ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
        ]))
        story.extend([value, Spacer(1, 12)])

    def formula(text, number):
        row = Table([[p(text, "Formula"), p(f"({number})", "FormulaNo")]],
                    colWidths=[doc.width - 45, 45])
        row.setStyle(TableStyle([("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
                                 ("LEFTPADDING", (0, 0), (-1, -1), 0), ("RIGHTPADDING", (0, 0), (-1, -1), 0)]))
        story.append(row)

    def code(label, relative, signature):
        story.append(p(label, "TableCaption"))
        story.append(Preformatted(excerpt(relative, signature), styles["Code"]))

    def chart(name, caption):
        drawing = drawings[name]
        factor = doc.width / drawing.width
        drawing.scale(factor, factor)
        drawing.width *= factor
        drawing.height *= factor
        story.append(KeepTogether([drawing, p(caption, "Caption")]))

    def figure(filename, caption, wide_page=False, max_height=500):
        image = Image(str(ROOT / "docs/diagrams" / filename))
        if wide_page:
            image._restrictSize(wide_size[0] - 40 * mm, max_height if max_height != 500 else 440)
        else:
            image._restrictSize(doc.width, max_height)
        image.hAlign = "CENTER"
        story.append(KeepTogether([image, p(caption, "Caption")]))

    def wide_figure(filename, caption):
        story.extend([NextPageTemplate("Landscape"), PageBreak()])
        figure(filename, caption, wide_page=True)

    # ------------------------------------------------------------------ cover
    story += [
        p("МІНІСТЕРСТВО ОСВІТИ І НАУКИ УКРАЇНИ", "CoverBold"),
        p("НАЦІОНАЛЬНИЙ ТЕХНІЧНИЙ УНІВЕРСИТЕТ УКРАЇНИ", "CoverBold"),
        p("«КИЇВСЬКИЙ ПОЛІТЕХНІЧНИЙ ІНСТИТУТ ІМЕНІ ІГОРЯ СІКОРСЬКОГО»", "CoverBold"),
        p("Навчально-науковий інститут прикладного системного аналізу", "Cover"),
        p("Кафедра системного проєктування", "Cover"),
        Spacer(1, 26 * mm),
        p("КУРСОВА РОБОТА", "CoverTitle"),
        p("з дисципліни «Паралельні обчислення»", "Cover"),
        Spacer(1, 6 * mm),
        p("на тему: «Дослідження та реалізація паралельних обчислень на прикладі побудови пошукової системи з інтеграцією LLM»", "CoverBold"),
        Spacer(1, 22 * mm),
        p("Виконавець:", "CoverRight"),
        p("здобувач вищої освіти IV курсу", "CoverRight"),
        p(f"{args.author}", "CoverRight"),
        p(f"Група: {args.group or '________'}", "CoverRight"),
        Spacer(1, 4 * mm),
        p("Керівник:", "CoverRight"),
        p("доктор філософії, старший викладач", "CoverRight"),
        p("Яременко Вадим Сергійович", "CoverRight"),
        Spacer(1, 24 * mm),
        p("Київ – 2026", "Cover"),
    ]

    # -------------------------------------------------------------- contents
    story.append(PageBreak())
    styles["TOCTitle"] = ParagraphStyle("TOCTitle", parent=styles["H1"])
    story.append(p("ЗМІСТ", "TOCTitle"))
    toc = TableOfContents()
    toc.levelStyles = [
        ParagraphStyle("TOC0", fontName="Report-Bold", fontSize=12, leading=14, spaceBefore=3),
        ParagraphStyle("TOC1", fontName="Report", fontSize=12, leading=13.5, leftIndent=16),
    ]
    story.append(toc)

    # ----------------------------------------------------------------- intro
    h1("Вступ")
    para("Обсяги текстових даних зростають швидше, ніж частота окремого ядра процесора, а пошук у великих колекціях не можна виконувати повторним переглядом усіх документів. Типове вирішення – інвертований індекс, який зіставляє кожному терміну список документів, де він трапляється. Його побудова добре розпаралелюється, бо документи можна обробляти незалежно, але спільну структуру, яку одночасно змінюють кілька потоків, необхідно ретельно синхронізувати. Додатково мовна модель може розширити запит синонімами, проте її виклик має високу й непередбачувану затримку та може бути недоступним, тож відмова моделі не повинна ламати основний пошук.")
    para("<b>Мета роботи</b> – реалізувати й дослідити контейнеризовану пошукову систему, яка ефективно використовує паралелізм: паралельну побудову інвертованого індексу власним пулом потоків, потокобезпечну структуру даних і одночасну обробку запитів кількох клієнтів, а також розширення запитів локальною мовною моделлю.")
    para("Для досягнення мети поставлено такі <b>завдання</b>:")
    bullets([
        "проаналізувати теорію індексу, синхронізації потоків і масштабованості;",
        "розробити UML-модель системи;",
        "реалізувати мовою C++ власні пул потоків і шардований інвертований індекс без готових бібліотечних аналогів;",
        "реалізувати REST API, WebSocket-стрімінг етапів пошуку, консольний клієнт і вебінтерфейс;",
        "інтегрувати локальну модель Ollama для розширення запитів із безпечним запасним варіантом;",
        "контейнеризувати систему для запуску командою docker compose up, виконати тестування й дослідити масштабованість.",
    ])
    para("<b>Об'єкт дослідження</b> – побудова та використання інвертованого індексу в багатопотоковій програмі. <b>Предмет</b> – вплив кількості потоків і кількості одночасних клієнтів на час побудови індексу та пропускну здатність пошуку. Методи: теоретичний аналіз, проєктування, реалізація, автоматизоване тестування й експеримент.")

    # ------------------------------------------------------------- chapter 1
    h1("Розділ 1. Теоретичні відомості та огляд технологій")
    h2("1.1. Пошукові системи та інвертований індекс")
    para("Задача інформаційного пошуку полягає у відборі з колекції документів, що відповідають запиту, і їх впорядкуванні за релевантністю. Послідовне читання всіх документів для кожного запиту має складність, пропорційну сумарному обсягу тексту, тому практичні системи будують індекс [2]. Інвертований індекс складається зі словника термінів і списків входжень (postings); кожен запис містить ідентифікатор документа та частоту терміна в ньому. Наприклад, список [(12, 3), (97, 1)] для терміна «reef» означає три входження в документі 12 і одне в документі 97. Обсяг роботи під час пошуку залежить від довжини списків, а не від розміру корпусу.")
    para("Побудова індексу включає читання документа, токенізацію, нормалізацію, підрахунок частот і запис у словник. Алгоритми для великих колекцій (BSBI, SPIMI) розглянуто в розділі «Index construction» книги [2]. Реалізована система за ідеєю близька до SPIMI: словник – хеш-таблиця в пам'яті, а списки зростають у міру обробки документів. Для ранжування використано вагову схему TF-IDF:")
    formula("w(t, d) = tf(t, d) / |d| · ln(1 + N / df(t))", "1.1")
    flat("де tf(t, d) – кількість входжень терміна в документ; |d| – довжина документа в токенах; N – кількість документів; df(t) – кількість документів, що містять термін. Оцінка документа для запиту q:")
    formula("score(q, d) = Σ w(t, d)  за всіма термінами t запиту q", "1.2")
    para("Рідкісні терміни дають більший внесок, ніж часті службові слова, а нормування на довжину знижує перевагу дуже довгих текстів. У роботі оцінку додатково ділять на найкращу, тому перший результат завжди має оцінку 1.")

    h2("1.2. Процеси, потоки та синхронізація")
    para("Процес – екземпляр програми з власним віртуальним адресним простором: код, дані, купа, стеки потоків і відображені файли. Віртуальну адресу відображає на фізичну апаратний модуль керування пам'яттю за таблицями сторінок ядра, що забезпечує ізоляцію процесів. Потік – одиниця планування всередині процесу: потоки ділять адресний простір і купу, але кожен має власний стек (окремий діапазон віртуальних адрес, типово порядку мегабайтів) і регістри. Тому локальні змінні потоку безпечні, а дані на купі й глобальні потребують синхронізації. Процеси обмінюються даними через канали, спільну пам'ять і сокети; сокет – кінцева точка обміну, що ідентифікується IP-адресою й портом (сервер цієї роботи слухає порт 8080).")
    para("Гонка даних (data race) виникає, коли два потоки без синхронізації звертаються до однієї пам'яті й принаймні один пише; у C++ це невизначена поведінка [7]. Такі помилки рідкісні й залежать від планування, тому для їх пошуку використовують динамічні аналізатори, зокрема ThreadSanitizer [13]. Взаємне блокування (deadlock) виникає за одночасної дії чотирьох умов: взаємного виключення, утримання з очікуванням, неможливості відібрати ресурс і циклічного очікування. У цій роботі застосовано просте правило: жодна операція індексу не тримає двох блокувань одночасно, тому цикл очікування неможливий.")
    para("Бібліотека C++ надає std::mutex, std::shared_mutex (багато читачів або один письменник), std::condition_variable (очікування без активного циклу, завжди з предикатом через хибні пробудження) та std::atomic [3, 4]. Неблокуючі алгоритми будуються на атомарних операціях порівняння з обміном: затримка одного потоку не зупиняє інші, а відсутність гонки забезпечує апаратна атомарність операції з визначеним впорядкуванням пам'яті. Складність коректної реалізації таких структур висока, тому для індексу обрано блокування дрібної гранулярності, а атомарні змінні – лише для лічильників і прапорців. За моделлю пам'яті C++ розблокування mutex «відбувається раніше» наступного блокування того самого mutex, тому опублікований під блокуванням індекс повністю видимий читачам.")

    h2("1.3. Пул потоків та оцінювання масштабованості")
    para("Створення потоку на кожне мале завдання дороге, а тисячі короткоживучих потоків перевантажують планувальник. Пул потоків запускає фіксовану кількість робітників, які в циклі беруть завдання зі спільної черги. Мінімальний пул складається з черги, захищеної mutex, умовної змінної та прапорця зупинки. Коректне завершення має спорожнити чергу, дочекатися виконуваних завдань і приєднати потоки; виняток у завданні не повинен завершувати робітника [4]. Паралельну обробку HTTP-запитів також будують на пулі: бібліотека Crow у багатопотоковому режимі запускає кілька потоків обробки введення-виведення, а тривалі операції в цій роботі винесено у власний пул.")
    para("Прискорення при p потоках і ефективність визначають так:")
    formula("S(p) = T(1) / T(p),   E(p) = S(p) / p", "1.3")
    para("Закон Амдала [11] пов'язує максимальне прискорення з часткою s послідовної частини програми:")
    formula("S(p) ≤ 1 / (s + (1 − s) / p)", "1.4")
    para("При s = 5 % прискорення не перевищує 20 за будь-якої кількості потоків. Закон Густафсона [12] додає, що за зростання обсягу задачі разом із потоками виграш може бути більшим. На практиці масштабування також обмежують пропускна здатність пам'яті, дисковий ввід-вивід, конкуренція за блокування й кількість фізичних ядер. Для сервера використовують запити за секунду (throughput) і процентилі затримки p50 та p95.")

    h2("1.4. REST, WebSocket, мовні моделі та контейнеризація")
    para("REST [8] – архітектурний стиль, у якому сервер надає ресурси за адресами, клієнт виконує стандартні операції HTTP, а кожен запит самодостатній; результат повідомляється кодом стану [10] (200, 400, 404, 405, 409). Протокол WebSocket [9] відкриває повнодуплексний канал поверх TCP після HTTP-запиту з заголовком Upgrade, тож сервер може надіслати кілька повідомлень у відповідь на один запит. Для пошуку з розширенням це зручно: клієнт одразу бачить розширення, а потім результати.")
    para("Розширення запиту (query expansion) додає синоніми й споріднені терміни, зменшуючи розрив між словником запиту та документів, але несе ризики дрейфу теми, помилкових слів і затримки генерації. Ollama – локальний сервер запуску мовних моделей із маршрутом POST /api/generate [14]; обрана модель qwen2.5:1.5b має близько 1,5 млрд параметрів, у квантованому вигляді займає близько 1 ГБ і працює на процесорі. Параметр temperature = 0 робить відповідь відтворюваною.")
    para("Контейнер – ізольований процес із власними файловою системою та мережею, який використовує ядро хоста; на відміну від віртуальної машини, він не містить окремої операційної системи. Багатоетапне збирання Dockerfile дозволяє зібрати програму в одному образі, а запускати в меншому [15]. Docker Compose описує сервіси, мережі, змінні й томи декларативно; сервіси однієї мережі звертаються один до одного за іменем (наприклад, http://ollama:11434), а іменовані томи зберігають модель і корпус між запусками.")
    h2("1.5. Висновки до розділу 1")
    para("Розглянуті положення визначили проєктні рішення: шардований індекс із дрібними блокуваннями, власний пул із завершенням після спорожнення черги, кешований стан зовнішньої моделі й запасний варіант пошуку без неї.")

    # ------------------------------------------------------------- chapter 2
    h1("Розділ 2. Модель системи")
    h2("2.1. Аналіз вимог")
    para("Вимоги сформовано за завданням курсової роботи [1]; кожну можна перевірити окремим автоматичним сценарієм. Нефункціональні вимоги: відсутність гонок і взаємних блокувань, стійкість до відмови Ollama, відтворюваність запуску й даних, безпека вебінтерфейсу (екранування виводу, локальні ресурси без CDN, непривілейований користувач у контейнері), єдиний стиль коду (clang-format за Google Style Guide [16]) і повідомлень комітів (Conventional Commits).")
    table(["Код", "Функціональна вимога", "Спосіб перевірки"], [
        ["FR1", "Запуск командою docker compose up; сервіси server і ollama; GET /api/health", "Compose, smoke-тест"],
        ["FR2", "Власний інвертований індекс із паралельним доступом", "Юніт- і стрес-тести, TSan"],
        ["FR3", "Паралельна побудова власним пулом; параметр threads", "Тести, експеримент E1"],
        ["FR4", "POST /api/search, режим keyword, фіксований JSON", "Тести REST-контракту"],
        ["FR5", "GET /api/stats: документи, час побудови, потоки", "Тести REST-контракту"],
        ["FR6", "Розширення запиту через Ollama /api/generate", "Тести зі стендом, реальна модель"],
        ["FR7", "WebSocket /ws/search: query_expansion, keyword_search, done", "Тести маршруту, клієнтський скрипт"],
        ["FR8", "Консольний клієнт search", "Python-сценарії CLI"],
        ["FR9", "Вебінтерфейс із візуалізацією етапів", "Node-тести, перевірка в браузері"],
        ["FR10", "Графіки: час побудови від потоків, throughput від клієнтів", "Експерименти E1 та E2"],
    ], [0.1, 0.58, 0.32], "2", "Функціональні вимоги")

    h2("2.2. Архітектура")
    para("Система має три шари. Транспортний (маршрути HTTP і WebSocket) перевіряє вхідні дані й формує відповіді. Сервісний містить IndexService (побудова й публікація індексу), SearchPipeline (розширення запиту та пошук) і SearchSocketService (потокові запити). Шар ядра містить власні ThreadPool, InvertedIndex, токенізатор і SearchEngine; окремо розташовано інтеграцію з Ollama. Код збирається в бібліотеку search_core та два виконувані файли: сервер search_server і клієнт search, що дозволяє тестувати ядро без HTTP-сервера. Залежності (Crow, cpp-httplib, nlohmann/json, GoogleTest) завантажує CMake із зафіксованими версіями.")

    h2("2.3. Варіанти використання")
    para("Виділено чотири сценарії (рис. 2.1): UC1 – побудова або перебудова індексу оператором; UC2 – REST-пошук користувачем консольного клієнта; UC3 – потоковий пошук користувачем вебінтерфейсу; UC4 – читання стану та статистики. Ollama бере участь у UC2 і UC3 як зовнішній постачальник розширення. Автоматичний запуск у Docker використовує той самий маршрут побудови індексу, що й UC1.")
    figure("use-case.png", "Рисунок 2.1 – Діаграма варіантів використання", max_height=300)

    h2("2.4. Діаграми класів")
    para("На діаграмі ядра (рис. 2.2) IndexService володіє опублікованим знімком індексу й створює ThreadPool на кожну побудову. InvertedIndex складається з масиву шардів, кожен із яких має власний std::shared_mutex і хеш-таблицю термінів. На діаграмі сервісного шару (рис. 2.3) маршрути API звертаються до IndexService, SearchPipeline і OllamaMonitor; SearchSocketService володіє власним ThreadPool; SearchPipeline викликає SearchEngine та, через функцію-розширювач, OllamaClient.")
    figure("class.png", "Рисунок 2.2 – Діаграма класів ядра", max_height=290)
    figure("class-services.png", "Рисунок 2.3 – Діаграма класів сервісного шару", max_height=400)

    h2("2.5. Діаграми послідовностей")
    para("Для кожного варіанта використання побудовано діаграму послідовності (рис. 2.4 – 2.7). У побудові індексу кожна задача читає один файл і викликає AddDocument; токенізація виконується поза блокуваннями, шарди блокуються по одному. У REST-пошуку, якщо модель недоступна або не вклалася в обмеження часу, пошук іде за оригінальним запитом. У WebSocket-пошуку обробник лише ставить завдання у власний пул, а етапи надсилаються по мірі готовності. Маршрут /api/health відповідає зі збереженого значення, яке оновлює фоновий потік.")
    wide_figure("sequence-index.png", "Рисунок 2.4 – Діаграма послідовності UC1 (побудова індексу)")
    wide_figure("sequence-rest.png", "Рисунок 2.5 – Діаграма послідовності UC2 (REST-пошук)")
    wide_figure("sequence-websocket.png", "Рисунок 2.6 – Діаграма послідовності UC3 (потоковий пошук)")
    wide_figure("sequence-health.png", "Рисунок 2.7 – Діаграма послідовності UC4 (стан і статистика)")

    story.extend([NextPageTemplate("Landscape"), PageBreak()])
    h2("2.6. Розгортання")
    figure("deployment.png", "Рисунок 2.8 – Діаграма розгортання Docker Compose", wide_page=True, max_height=390)
    story.extend([NextPageTemplate("Portrait"), PageBreak()])
    para("Діаграма розгортання (рис. 2.8) містить три контейнери: server (C++ сервер, вбудований вебінтерфейс, Python-супервізор), ollama (мовна модель) та одноразовий model-init, що завантажує й прогріває модель. Дані зберігаються у двох іменованих томах: generated_data (корпус і manifest) та ollama_models (ваги). HTTP-інтерфейс доступний до завершення підготовки корпусу, тому користувач одразу бачить статус підготовки.")
    h2("2.7. Стани індексу")
    para("Індекс має три стани, які повертає маршрут /api/ready. Пошук доступний лише в стані ready, а нова побудова – якщо поточна не виконується.")
    table(["Стан", "Умова", "Поведінка API"], [
        ["empty", "Індекс не будувався або перша побудова завершилася помилкою", "Пошук: 409 «index not built»; stats – нулі"],
        ["building", "Виконується побудова", "Пошук і повторна побудова: 409 «index is building»; health – 200"],
        ["ready", "Опубліковано повний індекс", "Пошук і stats працюють; можлива нова побудова"],
    ], [0.15, 0.4, 0.45], "2", "Стани індексу")
    para("Перевірка «чи триває побудова» і перехід між станами виконуються під одним mutex, тож дві побудови не можуть початися одночасно, а читач бачить індекс і статистику з однієї успішної побудови. Невдала повторна побудова не вимикає раніше опублікований індекс.")
    h2("2.8. Висновки до розділу 2")
    para("Побудовано повну UML-модель: варіанти використання, дві діаграми класів, чотири діаграми послідовностей і діаграму розгортання. Модель фіксує ключові рішення: ядро без залежності від мережі, публікацію індексу як незмінного знімка, власний пул для тривалих операцій WebSocket і окремий потік перевірки Ollama.")

    # ------------------------------------------------------------- chapter 3
    h1("Розділ 3. Опис реалізації")
    h2("3.1. Структура проєкту")
    table(["Каталог", "Призначення", "Рядків"], [
        ["src/", "Код сервера та клієнта (C++20): ядро, сервіси, інтеграції", num(loc["src"])],
        ["tests/", "Тести GoogleTest, Python та Node", num(loc["tests"])],
        ["web/", "Вебінтерфейс (HTML, JavaScript, CSS, локальний Vue)", num(loc["web"])],
        ["tools/, bench/, scripts/", "Генератор корпусу, експерименти, графіки, звіт, smoke-тест", num(loc["tools"])],
    ], [0.22, 0.58, 0.2], "3", "Структура репозиторію")

    h2("3.2. Власний пул потоків")
    para("ThreadPool (src/core/thread_pool.*) реалізовано на std::thread, std::deque, std::mutex і std::condition_variable. Submit під блокуванням додає завдання в чергу й сповіщає одного робітника; він повертає false, якщо пул зупиняється, завдання порожнє або черга заповнена. Робочий цикл (лістинг 3.1) очікує на умовній змінній із предикатом «пул зупиняється або черга непорожня», витягує завдання під блокуванням, але виконує його поза ним. Виняток завдання перехоплюється й записується в stderr одним записом під окремим mutex.")
    code("Лістинг 3.1 – Робочий цикл ThreadPool (src/core/thread_pool.cpp)", "src/core/thread_pool.cpp", "void ThreadPool::WorkerLoop()")
    para("Shutdown виставляє прапорець зупинки, будить робітників і приєднує потоки; робітники виходять лише з порожньою чергою, тому всі прийняті завдання виконуються. Приєднання відбувається під окремим mutex, а не під основним, інакше робітники не змогли б спорожнити чергу; завдяки цьому Shutdown безпечно викликати одночасно з кількох потоків і в деструкторі. Якщо конструктор не зміг створити всі потоки, створені зупиняються й приєднуються перед повторним викиданням винятку.")

    h2("3.3. Токенізація та шардований індекс")
    para("Один токенізатор обслуговує документи, запити й терміни від моделі. Токен – неперервна послідовність літер і цифр; роздільниками є пробіли, ASCII-пунктуація, типографські лапки й тире та некоректні UTF-8 послідовності. Регістр згортається для ASCII та кирилиці; стемінг і стоп-слова не застосовуються, тож пошук іде за точним токеном.")
    para("InvertedIndex має 32 шарди (задається конструктором); термін потрапляє в шард за хешем. Кожен шард має власний std::shared_mutex і хеш-таблицю від терміна до вектора Posting. Метадані документів захищено окремим mutex, лічильник записів – атомарний. AddDocument (лістинг 3.2) спершу рахує частоти локально без блокувань, потім резервує ідентифікатор документа, групує терміни за шардами й блокує кожен потрібний шард один раз на документ. Операція не тримає двох блокувань одночасно, тому взаємне блокування неможливе. Lookup бере спільне блокування одного шарда й повертає копію списку, тож читачі не блокують один одного.")
    code("Лістинг 3.2 – Додавання документа в індекс (src/core/inverted_index.cpp)", "src/core/inverted_index.cpp", "DocId InvertedIndex::AddDocument(")

    h2("3.4. Побудова та публікація індексу")
    para("IndexService::Build спочатку під mutex стану перевіряє, що побудова не триває, і виставляє стан building – обидві дії атомарні. Файли .txt перелічуються й сортуються для відтворюваних ідентифікаторів; кожен файл – окреме завдання власного пулу «прочитати – додати в індекс». Помилки читання не викидаються в робочому потоці, а зберігаються, і вся побудова завершується помилкою. Індекс будується окремо від опублікованого; після завершення всіх завдань індекс і статистика публікуються одним блоком під mutex як пара shared_ptr та BuildStats. Читач отримує або старий, або новий індекс цілком, але ніколи не напівзбудований; невдала повторна побудова залишає попередній індекс чинним.")

    h2("3.5. Пошук, ранжування та розширення запиту")
    para("SearchEngine розбирає запит тим самим токенізатором, видаляє дублікати термінів і для кожного документа сумує tf·idf за формулою (1.2). Оцінки діляться на найкращу; результати сортуються за оцінкою за спаданням, а за рівності – за doc_id, тож видача детермінована; повертається не більше 10 документів. Фрагмент (snippet) – вікно з 80 символів до і 120 після першого токена, що точно збігається з терміном запиту (тому «cat» не збігається з «education»); вікно не розрізає UTF-8 послідовності.")
    para("OllamaClient::ExpandQuery (лістинг 3.3) надсилає POST /api/generate із назвою моделі, інструкцією повернути синоніми, stream = false і temperature = 0. Відповідь очищається від маркерів списку, токенізується тим самим токенізатором; терміни, що вже є в запиті, і дублікати відкидаються, залишається не більше восьми нових. Обмеження часу генерації – 5 с на весь запит, а не лише на окремі операції читання, тож повільна модель не затримає пошук; за будь-якої помилки повертається порожній список.")
    code("Лістинг 3.3 – Розширення запиту (src/integrations/ollama_client.cpp)", "src/integrations/ollama_client.cpp", "std::vector<std::string> OllamaClient::ExpandQuery(")
    para("SearchPipeline викликає розширення до пошуку, повідомляє спостерігача про терміни (подія query_expansion), шукає за об'єднанням оригінальних і доданих термінів і повідомляє про результати (keyword_search). Для /api/health застосовано OllamaMonitor: фоновий потік раз на 5 с перевіряє /api/tags і зберігає результат в std::atomic<bool>, тож маршрут відповідає миттєво, навіть коли Ollama недоступна.")

    h2("3.6. REST API та WebSocket")
    table(["Маршрут", "Запит", "Відповідь"], [
        ["GET /api/health", "–", '{"status":"ok","ollama":true|false}'],
        ["POST /api/index", '{"threads":4}', '{"status":"ok","documents_indexed":N,"time_ms":T}'],
        ["POST /api/search", '{"query":"...","mode":"keyword"}', '{"results":[{"doc_id","score","snippet"}],"time_ms":T}'],
        ["GET /api/stats", "–", '{"documents":N,"index_build_time_ms":T,"threads_used":P}'],
        ["GET /api/ready", "–", "Додатково: index_state, search_ready, startup_state"],
        ["WS /ws/search", '{"query":"..."}', "query_expansion → keyword_search → done"],
    ], [0.19, 0.29, 0.52], "3", "Маршрути сервера")
    para("Параметр threads має діапазон 1–64 і значення за замовчуванням 4; некоректний JSON, порожній запит і режим, відмінний від keyword, дають 400; пошук і повторна побудова під час побудови – 409. Додатковий маршрут /api/ready введено, щоб не змінювати фіксований контракт /api/health.")
    para("Обробка WebSocket винесена з потоків введення-виведення: обробник перевіряє запит і ставить завдання у власний пул SearchSocketService (типово 4 робітники, черга на 64 завдання, 4 запити на з'єднання). Запити одного з'єднання виконуються в порядку надходження; при переповненні клієнт отримує server is busy, а поточна партія скасовується, щоб після помилки не надходили застарілі результати. Закриття з'єднання під mutex сесії забороняє подальшу відправку.")

    h2("3.7. Розгортання, дані та клієнти")
    para("Команда docker compose up запускає три сервіси. Python-супервізор у контейнері server одразу запускає C++ сервер, паралельно готує корпус і викликає POST /api/index; стан підготовки записується атомарною заміною JSON-файла й показується в /api/ready, а помилка підготовки не зупиняє HTTP. Сервіс model-init чекає на Ollama, за потреби завантажує модель і прогріває її.")
    para(f"Типовий корпус створює детермінований генератор на стандартній бібліотеці Python: {num(dataset['documents'])} документів, {num(dataset['bytes'])} байтів (зерно {dataset['seed']}). Файли пишуться в тимчасовий каталог, який перейменовується лише після успіху; manifest зберігає версію, зерно, розмір і SHA-256. <b>Цей корпус синтетичний</b>: він має різні теми й частоти слів, але не є природною мовою, тож фрагменти виглядають як набір випадкових слів. Це зроблено, щоб запуск із чистого клону не залежав від мережі та від даних, яких немає в Git. Перевірку на справжніх текстах через docker-compose.external.yml описано в підрозділі 4.7.")
    table(["Змінна", "За замовчуванням", "Призначення"], [
        ["PORT", "8080", "Порт HTTP-сервера"],
        ["DATA_DIR", "/data/generated/full", "Каталог із .txt-документами"],
        ["OLLAMA_URL, OLLAMA_MODEL", "http://ollama:11434, qwen2.5:1.5b", "Адреса й модель Ollama"],
        ["AUTO_INDEX, INDEX_THREADS", "true, 4", "Автопобудова при старті та кількість потоків"],
        ["QUERY_EXPANSION", "true", "false – лише для експериментів keyword-пошуку"],
        ["SEARCH_WORKERS, SEARCH_TASK_QUEUE", "4, 64", "Робітники й черга WebSocket-пошуку"],
    ], [0.3, 0.3, 0.4], "3", "Основні змінні середовища")
    para("Консольний клієнт search – окрема програма на C++, що викликає REST-пошук і друкує doc_id, score і фрагмент (параметри --server, --json, змінна SEARCH_URL; коди завершення 0 – успіх, 1 – помилка з'єднання чи відповіді, 2 – неправильні аргументи). Вебінтерфейс на Vue 3 зберігається локально й вбудовується в бінарник, тож звернень до CDN немає. Кожен пошук відкриває WebSocket-з'єднання; панель етапів показує стани «очікування», «виконується», «готово» й «помилка», а текст результатів виводиться інтерполяцією Vue з екрануванням, без v-html.")
    h2("3.8. Висновки до розділу 3")
    para("Реалізовано всі компоненти моделі. Власні пул потоків і шардований індекс не використовують готових бібліотечних аналогів; публікація індексу як незмінного знімка забезпечує узгоджене читання, а фоновий моніторинг Ollama й запасний варіант роблять систему стійкою до відмови моделі.")

    # ------------------------------------------------------------- chapter 4
    h1("Розділ 4. Тестування та результати")
    h2("4.1. Стратегія та результати автоматичних перевірок")
    para(f"Тестування включає юніт-тести компонентів, тести конкурентності, інтеграційні перевірки REST- і WebSocket-контрактів та сценарії клієнтів. Усі запускаються одним викликом CTest із жорстким лімітом 120 с на бінарник, тому взаємне блокування завершується помилкою, а не зависанням. Розподіл за наборами – у таблиці 4.1 (усього {sum(suites.values())}). Тести пулу перевіряють виконання тисяч завдань, порядок при одному потоці, роботу після винятку, відмову після зупинки та одночасну роботу кількох виробників і викликів Shutdown; тести індексу порівнюють багатопотоковий запис із послідовним.")
    table(["Набір", "К-сть", "Що перевіряється"],
          [[name, str(count), SUITE_NOTES.get(name, "")] for name, count in sorted(suites.items(), key=lambda item: -item[1])],
          [0.25, 0.1, 0.65], "4", "Набори автоматичних перевірок", keep=False)
    para(f"Перевірку виконано {verification['date']}: CTest – {verification['ctest_passed']} із {verification['ctest_total']} за {fx(verification['ctest_seconds'], 2)} с; ThreadSanitizer – {verification['tsan_passed']} із {verification['tsan_total']} за {fx(verification['tsan_seconds'], 2)} с, без повідомлень про гонки (запуск у Linux-контейнері з вимкненою рандомізацією адресного простору, setarch -R). Динамічний аналізатор знаходить гонки лише у виконаних сценаріях, тому відсутність повідомлень не доводить відсутність усіх гонок [13]. Форматування всіх C++ файлів перевірено clang-format у стилі Google [16].")
    table(["Сценарій", "Фактичний результат"], [
        ["Стандартний Compose", verification["compose"]],
        ["Холодний HTTP та готовність пошуку", comma(verification["cold_start"])],
        ["Справжня Ollama", verification["ollama"]],
        ["WebSocket", comma(verification["websocket"])],
        ["CLI", verification["cli"]],
        ["Браузер", verification["browser"]],
    ], [0.3, 0.7], "4", "Контрольні сценарії роботи сервісу")

    h2("4.2. Умови та методика експериментів")
    disk = environment.get("disk") or {}
    disk_text = f"{disk.get('FriendlyName', 'не зафіксовано')}, {disk.get('BusType', '')}"
    para(f"Вимірювання виконано {environment['started_utc'][:10]} на комп'ютері з процесором {environment['cpu_model']}; Docker бачить {environment['docker_cpus']} логічних процесорів і {fx(environment['docker_memory_bytes'] / 1024**3, 2)} ГіБ пам'яті, Docker Engine {environment['docker_version']}, Linux у WSL2. Клієнт навантаження запускався у {environment['client_platform']}; накопичувач – {disk_text}. Обмежень ресурсів контейнера не задавалося, файловий кеш ОС не очищувався.")
    para(f"Кожна точка – один неврахований прогрів і п'ять вимірюваних запусків. E1 вимірює повну перебудову індексу при 1, 2, 4, 8, 12 і 16 потоках (час – з поля time_ms сервера). E2 вимірює пропускну здатність пошуку при 1 – 64 одночасних клієнтах: {request_count} запитів у повторенні, кожен із новим HTTP-з'єднанням. Для E2 використано окремий сервер із вимкненим розширенням запиту, тож це час саме keyword-пошуку, а не повної затримки конвеєра. Значення p50 і p95 обчислюються в кожному повторенні, після чого береться медіана п'яти. Сирі дані, параметри середовища й набір запитів збережено в bench/results.")

    h2("4.3. Експеримент E1: паралельна побудова індексу")
    table(["Потоки", "Медіана, с", "Min – max, с", "S(p)", "E(p), %"], [
        [str(r["threads"]), f"{fx(r['median'] / 1000, 3)}", f"{fx(r['min'] / 1000, 3)} – {fx(r['max'] / 1000, 3)}",
         f"{fx(r['speedup'], 2)}", f"{fx(r['efficiency'] * 100, 1)}"] for r in e1
    ], [0.14, 0.2, 0.3, 0.16, 0.2], "4", "Час побудови індексу залежно від кількості потоків")
    chart("time", "Рисунок 4.1 – Час повної побудови: медіани та діапазони п'яти запусків")
    chart("speedup", "Рисунок 4.2 – Фактичне та ідеальне прискорення")
    para(f"Медіана часу зменшилася з {fx(e1[0]['median'] / 1000, 3)} с при одному потоці до {fx(best_index['median'] / 1000, 3)} с при {best_index['threads']} потоках; максимальне прискорення {fx(best_index['speedup'], 2)}, ефективність {fx(best_index['efficiency'] * 100, 1)} %. Критерій зменшення часу при збільшенні кількості потоків підтверджено. Ефективність знижується з ростом p: {fx(e1[1]['efficiency'] * 100, 0)} % при {e1[1]['threads']} потоках і {fx(e1[3]['efficiency'] * 100, 0)} % при {e1[3]['threads']}. Процесор має 6 фізичних ядер і 12 логічних, тому після 8 – 12 потоків виграш невеликий. " + (f"При {e1[-1]['threads']} потоках медіана навіть трохи гірша, ніж при {e1[-2]['threads']}, що узгоджується з накладними витратами планування та конкуренції." if e1[-1]['median'] > e1[-2]['median'] else "Подальше збільшення кількості потоків майже не зменшує час."))

    h2("4.4. Експеримент E2: пропускна здатність пошуку")
    table(["Клієнти", "Запитів/с", "p50, мс", "p95, мс", "Помилки"], [
        [str(r["clients"]), f"{fx(r['median'], 1)}", f"{fx(r['p50_ms'], 1)}", f"{fx(r['p95_ms'], 1)}", str(r["errors"])] for r in e2
    ], [0.16, 0.22, 0.2, 0.2, 0.22], "4", "Пропускна здатність keyword-пошуку залежно від кількості клієнтів")
    chart("throughput", "Рисунок 4.3 – Пропускна здатність (шкала клієнтів – log2)")
    chart("latency", "Рисунок 4.4 – Медіана p95 п'яти повторень (шкала клієнтів – log2)")
    para(f"Найбільша медіанна пропускна здатність – {fx(best_load['median'], 1)} запитів/с при {best_load['clients']} клієнтах проти {fx(e2[0]['median'], 1)} при одному. Пропускна здатність швидко зростає до {e2[3]['clients']} клієнтів ({fx(e2[0]['median'], 0)} → {fx(e2[3]['median'], 0)} запитів/с), потім виходить на плато, а p95 зростає від {fx(min(r['p95_ms'] for r in e2), 0)} до {fx(e2[-1]['p95_ms'], 0)} мс. Протягом {num(sum(r['runs'] for r in e2) * request_count)} виміряних запитів зафіксовано {sum(r['errors'] for r in e2)} помилок. Паралельні клієнти корисні до насичення ресурсів, після чого додаткове навантаження збільшує переважно час очікування.")

    h2("4.5. Аналіз вузьких місць")
    para("Під час побудови індексу є послідовні частини: перелік і сортування файлів, постановка завдань, публікація результату й звільнення попереднього покоління; паралельна частина має спільні точки конкуренції – mutex метаданих і найчастіші шарди. На масштабування впливають також пропускна здатність пам'яті, виділення пам'яті та читання файлів. Спостережуване S(p) < p узгоджується із законом Амдала (формула 1.4). У пошуку довжина кожного документа читається під спільним mutex метаданих, списки записів копіюються, а всі кандидати сортуються перед вибором перших десяти; частину вартості несе й клієнт навантаження, що працював на тому самому комп'ютері й відкривав нове з'єднання на запит.")
    para("Профілювання очікування на блокуваннях не виконувалося, тому ці пояснення є гіпотезами за структурою коду й формою графіків, а не доведеними причинами. Перспективні оптимізації – незмінні масиви метаданих у знімку індексу, вибір перших K результатів замість повного сортування та інструментація блокувань; їх ефект потрібно перевіряти новими експериментами.")

    h2("4.6. Тестові пошукові запити")
    table(["Запит", "Очікувано", "Фактично", "Перевірка"], [
        [r["query"], "Є результати" if r["expected"] == "nonempty" else "Немає збігів",
         f"{r['actual_count']} результатів", "Пройдено" if r["passed"] else "Помилка"] for r in cases
    ], [0.34, 0.24, 0.24, 0.18], "4", "Результати пошуку на тестових запитах")
    para("Контрольні фрази взято з manifest синтетичного корпусу; слово, якого немає в генераторі, – негативний випадок. Таблиця перевіряє знаходження відомих термінів, а не якість ранжування: на синтетичному корпусі precision і recall не мають змісту. Повні результати збережено в search_cases.json. Розширення запиту перевірено окремо з реальною моделлю: для «coral reef» вона додала, зокрема, ecosystem, marine і biodiversity, а порядок подій WebSocket був query_expansion, keyword_search, done.")

    h2("4.7. Перевірка на корпусі справжніх текстів")
    para(f"Щоб переконатися, що система працює не лише на синтетичних даних, виконано разовий запуск на справжніх текстах: книги Project Gutenberg з публічного набору deepmind-gutenberg нарізано скриптом tools/prepare_dataset.py на документи (кількість – {num(gutenberg['documents'])}, обсяг – {num(gutenberg['bytes'])} байт) і підключено через docker-compose.external.yml; розширення запиту вимкнено, індекс побудовано чотирма потоками.")
    table(["Параметр або запит", "Значення"],
          [["Документів / байт", f"{num(gutenberg['documents'])} / {num(gutenberg['bytes'])}"],
           ["Час побудови індексу (4 потоки)", f"{fx(gutenberg['index_time_ms'] / 1000, 1)} с"]] +
          [[f"Запит «{q['query']}»", f"{q['results']} результатів, keyword-етап {q['time_ms']} мс; перші: {', '.join(q['top_doc_ids'])}"]
           for q in gutenberg["queries"]],
          [0.38, 0.62], "4", "Одноразова перевірка на корпусі Project Gutenberg")
    four_threads = next(r for r in e1 if r["threads"] == 4)
    para(f"Для «whale» найвищі результати – фрагменти про китобійний промисел, що відповідає очікуванню. Це якісна перевірка працездатності, а не вимірювання продуктивності: час побудови {fx(gutenberg['index_time_ms'] / 1000, 1)} с значно більший за E1 (близько {fx(four_threads['median'] / 1000, 1)} с при 4 потоках), бо запуск був одиничним, із холодним кешем і каталогом, змонтованим із хоста Windows, тому його не можна порівнювати з таблицею 4.3. Фрагменти містять і службовий текст Gutenberg, бо частину заголовків скрипт підготовки не вилучає.")

    h2("4.8. Обмеження та висновки до розділу 4")
    bullets([
        "E1 і E2 виконано на синтетичному корпусі та одній апаратній конфігурації; на іншому процесорі, диску й корпусі числа будуть іншими.",
        "Навантаження генерувалося на тому самому комп'ютері; частота процесора й фонові процеси ОС не фіксувалися.",
        "E2 не включає розширення запиту; продуктивність мовної моделі потребує окремого експерименту.",
        "Якість ранжування на справжніх колекціях не оцінювалася (немає розмітки релевантності).",
    ])
    para(f"Автоматичні перевірки пройдено повністю ({verification['ctest_passed']} із {verification['ctest_total']}, у тому числі під ThreadSanitizer). Дослідження підтвердило зменшення часу побудови зі зростанням кількості потоків із прискоренням до {fx(best_index['speedup'], 2)} і показало межі масштабування; пропускна здатність пошуку досягає плато близько {fx(best_load['median'], 0)} запитів/с.")

    # ----------------------------------------------------------- conclusions
    h1("Висновки")
    para("У роботі реалізовано контейнеризовану пошукову систему мовою C++20 з власними пулом потоків і шардованим інвертованим індексом. Кількість робочих потоків параметризується; публікація індексу як незмінного знімка, дрібні блокування та правило «одне блокування на операцію» дозволяють безпечно поєднувати побудову й читання. REST API та WebSocket відповідають фіксованому контракту завдання, а консольний клієнт і вебінтерфейс дають два способи використання.")
    para(f"Експеримент на {num(dataset['documents'])} документах обсягом {fx(dataset['bytes'] / 1024**3, 2)} ГіБ показав прискорення побудови індексу до {fx(best_index['speedup'], 2)} разів при {best_index['threads']} потоках ({environment['docker_cpus']} логічних процесорів). Максимальна медіана пропускної здатності keyword-пошуку – {fx(best_load['median'], 1)} запитів/с, після чого зростання кількості клієнтів переважно збільшує затримку. Отже, результати показують користь паралелізму та його межі, а не необмежене лінійне масштабування.")
    para("Інтеграція локальної моделі Ollama розширює запити синонімами, а збій, недоступність або повільна відповідь моделі не ламають пошук завдяки запасному варіанту й фоновому моніторингу. Стандартний запуск автоматизує підготовку корпусу, індексацію й завантаження моделі; перший запуск потребує мережі для образів, залежностей і моделі. Напрями розвитку: зменшення конкуренції за метадані, вибір перших K результатів без повного сортування, вимірювання очікування на блокуваннях, дослідження повного конвеєра з моделлю та оцінка якості ранжування на розміченій колекції справжніх текстів.")

    # --------------------------------------------------------------- sources
    h1("Перелік використаних джерел")
    sources = [
        "Яременко В. С. Паралельні обчислення. Курсова робота: силабус. Київ: КПІ ім. Ігоря Сікорського, 2026.",
        "Manning C. D., Raghavan P., Schütze H. Introduction to Information Retrieval. Cambridge: Cambridge University Press, 2008. 506 p.",
        "Goetz B., Peierls T., Bloch J. et al. Java Concurrency in Practice. Boston: Addison-Wesley, 2006. 432 p.",
        "Williams A. C++ Concurrency in Action: Practical Multithreading. Shelter Island: Manning, 2012. 528 p.",
        "Мартін Р. Чистий код: створення, аналіз та рефакторинг. Харків: Фабула, 2019. 416 с.",
        "Семеренко В. П. Технології паралельних обчислень. Вінниця: ВНТУ, 2018. 105 с.",
        "ISO/IEC 14882:2020. Programming languages – C++. Geneva: ISO, 2020. https://eel.is/c++draft/",
        "Fielding R. T. Architectural Styles and the Design of Network-based Software Architectures: Ph.D. dissertation. Irvine: University of California, 2000.",
        "Fette I., Melnikov A. RFC 6455: The WebSocket Protocol. IETF, 2011.",
        "Fielding R., Nottingham M., Reschke J. RFC 9110: HTTP Semantics. IETF, 2022.",
        "Amdahl G. M. Validity of the single processor approach to achieving large scale computing capabilities // AFIPS Spring Joint Computer Conference. 1967. P. 483–485.",
        "Gustafson J. L. Reevaluating Amdahl's Law // Communications of the ACM. 1988. Vol. 31, No. 5. P. 532–533.",
        "Serebryany K., Iskhodzhanov T. ThreadSanitizer: data race detection in practice // WBIA. 2009.",
        "Ollama documentation. API reference: Generate a completion. https://docs.ollama.com/api",
        "Docker documentation. Compose file reference. https://docs.docker.com/reference/compose-file/",
        "Google C++ Style Guide. https://google.github.io/styleguide/cppguide.html",
        "Репозиторій коду, тестів, документації та результатів вимірювань. https://github.com/voloshyyn/parallel-coursework",
    ]
    for number, source in enumerate(sources, start=1):
        story.append(Paragraph(rich(f"{number}. {source}"), styles["Source"]))

    # -------------------------------------------------------------- appendices
    h1("Додаток А. Специфікація API")
    para("Усі відповіді сервера мають формат JSON. Маршрут /api/ready та повідомлення про помилки не входять до фіксованого контракту завдання і є додатковими домовленостями.")
    table(["Ситуація", "HTTP", "Тіло відповіді"], [
        ["Успішний запит", "200", "Див. таблицю 3.2"],
        ["Некоректний JSON, відсутнє або неправильне поле, threads поза межами 1–64", "400", '{"status":"error","message":"..."}'],
        ["Невідомий маршрут", "404", "Текст «404 Not Found»"],
        ["Метод не підтримується маршрутом", "405", "Текст «405 Method Not Allowed»"],
        ["Пошук або повторна побудова під час побудови; пошук до першої побудови", "409", '"index is building" або "index not built"'],
        ["Внутрішня помилка (наприклад, недоступний каталог датасету)", "500", '{"status":"error","message":"..."}'],
    ], [0.5, 0.1, 0.4], "А", "Коди відповідей та формат помилок")
    para("Повідомлення WebSocket при помилці: {\"stage\":\"error\",\"data\":{\"message\":\"...\"}}; після нього done не надсилається. Тексти помилок збігаються з REST (invalid JSON body, query must be a non-empty string, index not built, index is building, server is busy).")
    story.append(Preformatted(
        'curl http://localhost:8080/api/health\n'
        'curl -X POST http://localhost:8080/api/index -H "Content-Type: application/json" -d \'{"threads": 8}\'\n'
        'curl http://localhost:8080/api/stats\n'
        'curl -X POST http://localhost:8080/api/search -H "Content-Type: application/json" \\\n'
        '     -d \'{"query": "coral reef", "mode": "keyword"}\'',
        styles["Code"]))

    h1("Додаток Б. Інструкція зі збирання та запуску", new_page=False)
    para("Потрібні Docker з Compose; для збирання без Docker – компілятор C++20, CMake 3.22 або новіший, Asio та Git; для підготовки корпусу Gutenberg і формування звіту – Python 3.10 або новіший.")
    story.append(Preformatted(
        '# Запуск системи та клієнт\n'
        'docker compose up --build -d\n'
        'docker compose exec server search "coral reef"\n'
        '# Тести (Linux-контейнер); для TSan: --build-arg ENABLE_TSAN=ON\n'
        'docker build --target test -t coursework-tests .\n'
        'docker run --rm coursework-tests\n'
        '# TSan: запуск із --security-opt seccomp=unconfined та setarch -R ctest ...\n'
        '# Власний каталог документів замість синтетичного корпусу\n'
        'EXTERNAL_DATASET_PATH=/абсолютний/шлях docker compose \\\n'
        '    -f docker-compose.yml -f docker-compose.external.yml up --build -d',
        styles["Code"]))

    doc.multiBuild(story)
    print(OUTPUT)


if __name__ == "__main__":
    main()
