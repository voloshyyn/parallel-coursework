#!/usr/bin/env python3
"""Build vector charts and summary from measured CSVs (requires reportlab)."""
import csv
import json
import math
from pathlib import Path
import statistics

from reportlab.graphics import renderPDF, renderSVG
from reportlab.graphics.charts.lineplots import LinePlot
from reportlab.graphics.shapes import Drawing, Line, String
from reportlab.graphics.widgets.markers import makeMarker
from reportlab.lib import colors
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont

ROOT = Path(__file__).resolve().parents[1]
NAVY = colors.HexColor("#183d62")
TEAL = colors.HexColor("#087e8b")


def register_fonts():
    """Register Times New Roman (or a metric-compatible serif) as the report font.

    The first available family wins: Times New Roman on Windows/macOS, then
    Liberation Serif (metric-compatible with Times New Roman) and DejaVu Serif
    on Linux. A monospaced face is registered for code listings.
    """
    win = Path("C:/Windows/Fonts")
    mac = Path("/Library/Fonts")
    liberation = Path("/usr/share/fonts/truetype/liberation")
    dejavu = Path("/usr/share/fonts/truetype/dejavu")
    candidates = [
        (win / "times.ttf", win / "timesbd.ttf", win / "timesi.ttf", win / "timesbi.ttf"),
        (mac / "Times New Roman.ttf", mac / "Times New Roman Bold.ttf",
         mac / "Times New Roman Italic.ttf", mac / "Times New Roman Bold Italic.ttf"),
        (liberation / "LiberationSerif-Regular.ttf", liberation / "LiberationSerif-Bold.ttf",
         liberation / "LiberationSerif-Italic.ttf", liberation / "LiberationSerif-BoldItalic.ttf"),
        (dejavu / "DejaVuSerif.ttf", dejavu / "DejaVuSerif-Bold.ttf",
         dejavu / "DejaVuSerif-Italic.ttf", dejavu / "DejaVuSerif-BoldItalic.ttf"),
    ]
    for regular, bold, italic, bold_italic in candidates:
        if all(path.exists() for path in (regular, bold, italic, bold_italic)):
            pdfmetrics.registerFont(TTFont("Report", str(regular)))
            pdfmetrics.registerFont(TTFont("Report-Bold", str(bold)))
            pdfmetrics.registerFont(TTFont("Report-Italic", str(italic)))
            pdfmetrics.registerFont(TTFont("Report-BoldItalic", str(bold_italic)))
            pdfmetrics.registerFontFamily("Report", normal="Report", bold="Report-Bold",
                                          italic="Report-Italic", boldItalic="Report-BoldItalic")
            break
    else:
        raise RuntimeError("Install Times New Roman, Liberation Serif or DejaVu Serif "
                           "to render Ukrainian text")
    mono_candidates = [win / "cour.ttf", liberation / "LiberationMono-Regular.ttf",
                       dejavu / "DejaVuSansMono.ttf"]
    for mono in mono_candidates:
        if mono.exists():
            pdfmetrics.registerFont(TTFont("Report-Mono", str(mono)))
            return
    raise RuntimeError("Install Courier New, Liberation Mono or DejaVu Sans Mono")


def aggregate(filename, key, metric):
    with filename.open(encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    result = []
    for point in sorted({int(row[key]) for row in rows}):
        group = [row for row in rows if int(row[key]) == point]
        if len(group) < 5:
            raise RuntimeError(f"{filename}: fewer than five measured runs at {point}")
        values = [float(row[metric]) for row in group]
        item = {key: point, "median": statistics.median(values), "min": min(values),
                "max": max(values), "runs": len(values)}
        if key == "clients":
            item.update(p50_ms=statistics.median(float(row["p50_ms"]) for row in group),
                        p95_ms=statistics.median(float(row["p95_ms"]) for row in group),
                        errors=sum(int(row["errors"]) for row in group))
        result.append(item)
    return result


def chart(points, title, ylabel, xlabel, filename, ideal=False, xlog=False):
    drawing = Drawing(490, 290)
    plot = LinePlot()
    plot.x, plot.y, plot.width, plot.height = 55, 52, 410, 185
    transform = math.log2 if xlog else lambda value: value
    plot.data = [[(transform(x), median) for x, median, _, _ in points]]
    if ideal:
        plot.data.append([(0, 0), (points[-1][0], points[-1][0])])
    maximum = max(high for _, _, _, high in points)
    maximum = max(maximum, points[-1][0]) if ideal else maximum
    step = 5 if maximum <= 30 else (50 if maximum <= 250 else 200)
    ymax = math.ceil(maximum * 1.12 / step) * step
    plot.xValueAxis.valueMin = 0
    plot.xValueAxis.valueMax = transform(points[-1][0])
    plot.xValueAxis.valueSteps = [transform(point[0]) for point in points]
    if xlog:
        plot.xValueAxis.labelTextFormat = lambda value: str(round(2 ** value))
    plot.yValueAxis.valueMin = 0
    plot.yValueAxis.valueMax = ymax
    plot.yValueAxis.valueStep = step
    for axis in [plot.xValueAxis, plot.yValueAxis]:
        axis.labels.fontName = "Report"
        axis.labels.fontSize = 9
        axis.strokeColor = colors.HexColor("#778899")
    plot.yValueAxis.visibleGrid = True
    plot.yValueAxis.gridStrokeColor = colors.HexColor("#e5e9ed")
    plot.lines[0].strokeColor = NAVY
    plot.lines[0].strokeWidth = 2
    plot.lines[0].symbol = makeMarker("FilledCircle")
    plot.lines[0].symbol.fillColor = NAVY
    plot.lines[0].symbol.size = 5
    if ideal:
        plot.lines[1].strokeColor = TEAL
        plot.lines[1].strokeDashArray = [4, 3]
    drawing.add(plot)
    for x, median, low, high in points:
        px = plot.x + transform(x) / transform(points[-1][0]) * plot.width
        ylow, yhigh = (plot.y + y / ymax * plot.height for y in [low, high])
        for line in [Line(px, ylow, px, yhigh), Line(px - 3, ylow, px + 3, ylow),
                     Line(px - 3, yhigh, px + 3, yhigh)]:
            line.strokeColor = NAVY
            drawing.add(line)
    drawing.add(String(245, 270, title, textAnchor="middle", fontName="Report-Bold", fontSize=12))
    drawing.add(String(55, 245, ylabel, fontName="Report", fontSize=9))
    drawing.add(String(260, 27, xlabel, textAnchor="middle", fontName="Report", fontSize=10))
    legend = "Медіана 5 запусків; вуса: min-max"
    if ideal:
        legend += "; пунктир: ідеальне прискорення"
    drawing.add(String(245, 8, legend, textAnchor="middle", fontName="Report", fontSize=8))
    renderSVG.drawToFile(drawing, str(filename.with_suffix(".svg")))
    renderPDF.drawToFile(drawing, str(filename.with_suffix(".pdf")))
    try:
        import pypdfium2
        doc = pypdfium2.PdfDocument(str(filename.with_suffix(".pdf")))
        doc[0].render(scale=2).to_pil().save(filename.with_suffix(".png"))
        doc.close()
    except ImportError:
        pass  # SVG and PDF remain the lossless, shareable outputs.
    return drawing


def build():
    register_fonts()
    results = ROOT / "bench/results"
    plots = ROOT / "bench/plots"
    plots.mkdir(parents=True, exist_ok=True)
    e1 = aggregate(results / "e1.csv", "threads", "time_ms")
    e2 = aggregate(results / "e2.csv", "clients", "throughput_rps")
    baseline = e1[0]["median"]
    for item in e1:
        item["speedup"] = baseline / item["median"]
        item["efficiency"] = item["speedup"] / item["threads"]
    summary = {"index": e1, "throughput": e2}
    (results / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    environment = json.loads((results / "environment.json").read_text(encoding="utf-8"))
    cases = json.loads((results / "search_cases.json").read_text(encoding="utf-8"))
    best_index = min(e1, key=lambda row: row["median"])
    best_load = max(e2, key=lambda row: row["median"])
    lines = [
        "# Результати дослідження масштабованості", "",
        f"Дата: {environment['started_utc'][:10]}. CPU: {environment['cpu_model']}; "
        f"{environment['docker_cpus']} логічних CPU, {environment['docker_memory_bytes']/1024**3:.2f} GiB RAM у Docker.",
        f"Корпус: {environment['dataset']['documents']} документів, {environment['dataset']['bytes']} байтів, "
        f"seed={environment['dataset']['seed']}.",
        "Один неврахований прогрів і п'ять повторень на точку; прогрітий кеш ОС.",
        "Стовпчики min-max зберігають усі повільні запуски. Параметри середовища, стан Git і digest образу: "
        "[environment.json](../bench/results/environment.json). Методика: [bench/README.md](../bench/README.md).", "",
        "## E1. Побудова повного індексу", "",
        "| Потоки | Медіана, с | Min-max, с | Прискорення | Ефективність, % |",
        "|---:|---:|---:|---:|---:|",
    ]
    lines += [f"| {r['threads']} | {r['median']/1000:.3f} | {r['min']/1000:.3f}-{r['max']/1000:.3f} | "
              f"{r['speedup']:.2f} | {r['efficiency']*100:.1f} |" for r in e1]
    lines += ["", "![Час індексації](../bench/plots/index-time.png)", "",
              "![Прискорення](../bench/plots/index-speedup.png)", "",
              f"Найкраща медіана в цьому досліді: {best_index['median']/1000:.3f} с при "
              f"{best_index['threads']} потоках, S={best_index['speedup']:.2f}. Зменшення часу підтверджено вимірюваннями.",
              "Сирі дані: [e1.csv](../bench/results/e1.csv).", "",
              "## E2. Пропускна здатність keyword-пошуку", "",
              "Окремий сервер з QUERY_EXPANSION=false; звичайний Compose використовує true.",
              f"По {environment['options']['requests']} нових HTTP-з'єднань на повторення; p50/p95 — медіани метрик п'яти повторень.", "",
              "| Клієнти | Запити/с | p50, мс | p95, мс | Помилки |", "|---:|---:|---:|---:|---:|"]
    lines += [f"| {r['clients']} | {r['median']:.1f} | {r['p50_ms']:.1f} | {r['p95_ms']:.1f} | {r['errors']} |" for r in e2]
    lines += ["", "![Throughput](../bench/plots/throughput.png)", "",
              "![p95](../bench/plots/latency-p95.png)", "",
              f"Максимальна медіана: {best_load['median']:.1f} запитів/с при {best_load['clients']} клієнтах. "
              "Після насичення throughput не зростає пропорційно клієнтам, а p95 збільшується.",
              "Вісь кількості клієнтів на графіках — log2. Сирі дані: [e2.csv](../bench/results/e2.csv).", "",
              "## E3. Аналіз вузьких місць", "",
              "Побудова має послідовні перелік/сортування файлів, диспетчеризацію, публікацію та звільнення "
              "старого покоління. Популярні шарди і mutex метаданих створюють конкуренцію; важливі також "
              "алокації, пам'ять та I/O. S(p)<p узгоджується з моделлю Амдала, але не визначає окремо частку кожного чинника.", "",
              "Під час пошуку довжини документів читаються під metadata mutex; postings копіюються, всі кандидати "
              "сортуються до вибору top-10. Це можливі причини насичення разом з CPU/пам'яттю та HTTP-клієнтом. "
              "Профілювання очікування блокувань не виконувалось: наведені причинні пояснення є гіпотезами за кодом і графіками.", "",
              "Можливі наступні оптимізації: незмінні метадані snapshot, частковий top-K, інструментація блокувань. "
              "Їх ефективність потребує окремих вимірювань. Синтетичний корпус не моделює природні книги; "
              "частота CPU, температура й фонові процеси ОС не фіксувалися. LLM-throughput цим E2 не вимірюється.", "",
              "## Тестові пошукові запити", "",
              "| Запит | Очікування | Фактично | Результат |", "|---|---|---|---|"]
    lines += [f"| {r['query']} | {'Є збіги' if r['expected']=='nonempty' else 'Немає збігів'} | "
              f"{r['actual_count']} | {'Пройдено' if r['passed'] else 'Помилка'} |" for r in cases]
    lines += ["", "Контрольні фрази взято з manifest, невідомий токен — негативний випадок. "
              "Це перевірка відомих збігів, не оцінка precision/recall на незалежному корпусі. "
              "Повні фактичні результати: [search_cases.json](../bench/results/search_cases.json).", ""]
    (ROOT / "docs/experiments.md").write_text("\n".join(lines), encoding="utf-8")
    drawings = {}
    drawings["time"] = chart([(r["threads"], r["median"] / 1000, r["min"] / 1000, r["max"] / 1000) for r in e1],
                              "Час побудови повного індексу", "Час, с", "Кількість потоків", plots / "index-time")
    drawings["speedup"] = chart([(r["threads"], r["speedup"], baseline / r["max"], baseline / r["min"]) for r in e1],
                                 "Прискорення побудови індексу", "S(p) = T(1) / T(p)", "Кількість потоків", plots / "index-speedup", True)
    drawings["throughput"] = chart([(r["clients"], r["median"], r["min"], r["max"]) for r in e2],
                                    "Пропускна здатність keyword-пошуку", "Запити/с", "Одночасні клієнти (шкала log2)", plots / "throughput", xlog=True)
    drawings["latency"] = chart([(r["clients"], r["p95_ms"], r["p95_ms"], r["p95_ms"]) for r in e2],
                                 "Затримка keyword-пошуку", "Медіана p95, мс", "Одночасні клієнти (шкала log2)", plots / "latency-p95", xlog=True)
    return summary, drawings


if __name__ == "__main__":
    build()
