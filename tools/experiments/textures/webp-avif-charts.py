"""Charts for docs/experiments/webp-avif.md#rendered-models, as SVG rendered to PNG by vips.

    python3 -I tools/experiments/textures/webp-avif-charts.py RUN_DIR SWEEP_DIR OUT_DIR

- effort-own.png: bytes at equal rendered SSIMULACRA2 against the same codec's slowest effort,
  by encode time of a 2k image, for each effort of the sweep.
- effort-webp6.png: the same against WebP effort 6, both codecs on one scale.
- views.png: AVIF bytes over WebP bytes at equal score, per view, for the three settings of the
  main run.

Ratios and times are skewed, so points are geometric means and error bars span one geometric
standard deviation either way (×/÷), over models for ratios and over encoded 2k images for times.
Each model contributes one ratio: the median of its interpolated ratios over views and WebP quality
points, so that its four views and four qualities don't count as independent samples. Bars past
the axis are clipped, with their end written next to an arrow. Colours: the dataviz reference palette's first two categorical slots (blue
WebP, orange AVIF), checked with its validator; marks differ by shape too (circle, square).
"""
import csv
import math
import os
import statistics
import subprocess
import sys
from collections import defaultdict

RUN, SWEEP, OUT = sys.argv[1:4]
COLOR = {"webp": "#2a78d6", "avif": "#eb6834"}
NAME = {"webp": "WebP", "avif": "AVIF"}
INK, INK2, MUTED, GRID, SURFACE = "#0b0b0b", "#52514e", "#8a8984", "#e4e3df", "#fcfcfb"
VIEW_NAMES = {"full": "whole model", "near": "3× close-up", "back-near": "3× close-up, back", "close": "6× close-up"}


def load(directory):
    curves = defaultdict(list)  # (model, view, mode, codec, effort) -> [(bytes, score)]
    for r in csv.DictReader(open(os.path.join(directory, "scores.csv"))):
        if r["codec"] != "reference":
            curves[(r["model"], r["view"], r["mode"], r["codec"], r["effort"])].append((int(r["bytes"]), float(r["ssimulacra2"])))
    times = defaultdict(list)
    for r in csv.DictReader(open(os.path.join(directory, "encodes.csv"))):
        if r["mode"] == "fast":
            times[(r["codec"], r["effort"])].append(float(r["seconds"]))
    return curves, times


def bytes_at(points, score):
    pts = sorted(points, key=lambda p: p[1])
    for (b0, s0), (b1, s1) in zip(pts, pts[1:]):
        if s0 <= score <= s1 and s1 > s0:
            return math.exp(math.log(b0) + (score - s0) / (s1 - s0) * (math.log(b1) - math.log(b0)))
    return None


def per_model(curves, mode, setting, baseline, views=None):
    """{model: median over views and baseline points of setting bytes / baseline bytes, same score}."""
    values = defaultdict(list)
    for (model, view, m, codec, effort), points in curves.items():
        if m != mode or (codec, effort) != baseline or (views and view not in views):
            continue
        other = curves.get((model, view, mode, *setting), [])
        for b, s in points:
            x = bytes_at(other, s)
            if x is not None:
                values[model].append(x / b)
    return {m: statistics.median(v) for m, v in values.items() if v}


def geometric(values):
    """(geometric mean, mean / GSD, mean × GSD)."""
    logs = [math.log(v) for v in values]
    m, sd = statistics.mean(logs), (statistics.stdev(logs) if len(logs) > 1 else 0)
    return math.exp(m), math.exp(m - sd), math.exp(m + sd)


# ---- SVG helpers -------------------------------------------------------------------------------

def text(x, y, s, size=13, color=INK, anchor="start", weight="normal"):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{color}" text-anchor="{anchor}" '
            f'font-weight="{weight}" font-family="DejaVu Sans, sans-serif">{s}</text>')


def marker(codec, x, y):
    c = COLOR[codec]
    if codec == "webp":
        return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" fill="{c}" stroke="{SURFACE}" stroke-width="2"/>'
    return f'<rect x="{x - 5:.1f}" y="{y - 5:.1f}" width="10" height="10" rx="1.5" fill="{c}" stroke="{SURFACE}" stroke-width="2"/>'


def write(name, width, height, body):
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">'
           f'<rect width="{width}" height="{height}" fill="{SURFACE}"/>{"".join(body)}</svg>')
    path = os.path.join(OUT, name)
    open(path[:-4] + ".svg", "w").write(svg)
    subprocess.run(["vips", "copy", path[:-4] + ".svg[dpi=144]", path], check=True)
    os.remove(path[:-4] + ".svg")


def vbar(x, lo, hi, y0, y1, py, color):
    """A vertical error bar from lo to hi, clipped to [y0, y1] with an arrow and the true end."""
    out = [f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{py(max(lo, y0)):.1f}" y2="{py(min(hi, y1)):.1f}" stroke="{color}" stroke-width="1.5"/>']
    if hi > y1:
        out.append(f'<path d="M{x - 5:.1f},{py(y1) + 7:.1f} L{x:.1f},{py(y1):.1f} L{x + 5:.1f},{py(y1) + 7:.1f}" fill="none" stroke="{color}" stroke-width="1.5"/>')
        out.append(text(x + 8, py(y1) + 10, f"to {hi:.2f}", 11, INK2))
    if lo < y0:
        out.append(f'<path d="M{x - 5:.1f},{py(y0) - 7:.1f} L{x:.1f},{py(y0):.1f} L{x + 5:.1f},{py(y0) - 7:.1f}" fill="none" stroke="{color}" stroke-width="1.5"/>')
        out.append(text(x + 8, py(y0) - 4, f"to {lo:.2f}", 11, INK2))
    return out


def effort_chart(name, title, subtitle, curves, times, baseline_of, threshold=None, y_max=1.5):
    width, height = 860, 520
    left, right, top, bottom = 80, 40, 112, 70
    settings = sorted({(k[3], k[4]) for k in curves if k[2] == "fast"}, key=lambda s: (s[0] != "webp", int(s[1])))
    points = {}
    for codec, effort in settings:
        ratios = per_model(curves, "fast", (codec, effort), baseline_of(codec))
        if len(ratios) > 1:
            points[(codec, effort)] = (geometric(times[(codec, effort)]), geometric(ratios.values()))
    xs = [t[2] for t, _ in points.values()]
    ys = [r[1] for _, r in points.values()] + [min(r[2], y_max) for _, r in points.values()]
    x0, x1 = math.log10(0.1), math.log10(max(xs) * 1.15)
    y0, y1 = math.floor(min(ys + [1.0]) * 20) / 20 - 0.05, min(y_max, math.ceil(max(ys + [1.0]) * 20) / 20 + 0.05)

    def px(t):
        return left + (math.log10(max(t, 0.1)) - x0) / (x1 - x0) * (width - left - right)

    def py(r):
        return top + (y1 - r) / (y1 - y0) * (height - top - bottom)

    body = [text(left, 30, title, 17, weight="bold")] + [text(left, 52 + 18 * i, line, 13, INK2) for i, line in enumerate(subtitle.split("\n"))]
    # Grid and axes.
    step = 0.05 if y1 - y0 <= 0.6 else 0.1
    r = math.ceil(y0 / step) * step
    while r <= y1 + 1e-9:
        body.append(f'<line x1="{left}" x2="{width - right}" y1="{py(r):.1f}" y2="{py(r):.1f}" stroke="{GRID}" stroke-width="1"/>')
        body.append(text(left - 8, py(r) + 4, f"{r:.2f}", 12, INK2, "end"))
        r += step
    for t in [0.1, 0.2, 0.5, 1, 2, 5, 10]:
        if x0 <= math.log10(t) <= x1:
            body.append(f'<line x1="{px(t):.1f}" x2="{px(t):.1f}" y1="{top}" y2="{height - bottom}" stroke="{GRID}" stroke-width="1"/>')
            body.append(text(px(t), height - bottom + 18, f"{t:g} s", 12, INK2, "middle"))
    body.append(text((left + width - right) / 2, height - 22, "Encode time of a 2k image (wall, 4 cores, log scale)", 13, INK2, "middle"))
    body.append(f'<line x1="{left}" x2="{width - right}" y1="{py(1):.1f}" y2="{py(1):.1f}" stroke="{MUTED}" stroke-width="1.5"/>')
    if threshold:
        body.append(f'<line x1="{left}" x2="{width - right}" y1="{py(threshold):.1f}" y2="{py(threshold):.1f}" '
                    f'stroke="{MUTED}" stroke-width="1.5" stroke-dasharray="5 4"/>')
        body.append(text(left + 6, py(threshold) - 6, f"+{(threshold - 1) * 100:.0f} %: mostly good", 12, INK2))
    # Series: lines through the means, then error bars, then markers and labels.
    for codec in ("webp", "avif"):
        series = sorted(((int(e), v) for (c, e), v in points.items() if c == codec), key=lambda p: p[0])
        path = " ".join(f"{'M' if i == 0 else 'L'}{px(t[0]):.1f},{py(min(r[0], y1)):.1f}" for i, (_, (t, r)) in enumerate(series))
        body.append(f'<path d="{path}" fill="none" stroke="{COLOR[codec]}" stroke-width="2" stroke-opacity="0.6"/>')
        for effort, ((tm, tlo, thi), (rm, rlo, rhi)) in series:
            x, y = px(tm), py(min(rm, y1))
            body.extend(vbar(x, rlo, rhi, y0, y1, py, COLOR[codec]))
            body.append(f'<line x1="{px(tlo):.1f}" x2="{px(thi):.1f}" y1="{y:.1f}" y2="{y:.1f}" stroke="{COLOR[codec]}" stroke-width="1.5"/>')
        for effort, ((tm, tlo, thi), (rm, rlo, rhi)) in series:
            body.append(marker(codec, px(tm), py(min(rm, y1))))
            if codec == "webp":
                body.append(text(px(tm) - 9, py(min(rm, y1)) - 9, f"{effort}", 13, INK, "end", "bold"))
            else:
                body.append(text(px(tm) + 9, py(min(rm, y1)) + 20, f"{effort}", 13, INK, "start", "bold"))
    # Legend.
    lx = left
    for codec in ("webp", "avif"):
        body.append(marker(codec, lx + 6, 94))
        body.append(text(lx + 18, 98, f"{NAME[codec]} (labels: effort)", 13, INK))
        lx += 210
    write(name, width, height, body)
    return points


def views_chart(name, curves_run):
    width, height = 860, 420
    left, right, top, bottom = 80, 40, 84, 60
    groups = [("Full size, default effort", "full", ("avif", "4"), ("webp", "4")),
              ("2k, effort 0", "fast", ("avif", "0"), ("webp", "0")),
              ("2k, AVIF 3 vs WebP 6", "fast", ("avif", "3"), ("webp", "6"))]
    views = ["full", "near", "back-near", "close"]
    y0, y1 = 0.4, 1.2

    def py(r):
        return top + (y1 - r) / (y1 - y0) * (height - top - bottom)

    body = [text(left, 30, "AVIF size relative to WebP at equal rendered quality", 17, weight="bold"),
            text(left, 52, "AVIF bytes ÷ WebP bytes at the same SSIMULACRA2 of the render; below 1, AVIF is smaller.", 13, INK2),
            text(left, 70, "Geometric mean ×/÷ geometric SD over 16 models.", 13, INK2)]
    r = 0.4
    while r <= y1 + 1e-9:
        body.append(f'<line x1="{left}" x2="{width - right}" y1="{py(r):.1f}" y2="{py(r):.1f}" stroke="{GRID}" stroke-width="1"/>')
        body.append(text(left - 8, py(r) + 4, f"{r:.1f}", 12, INK2, "end"))
        r += 0.1
    body.append(f'<line x1="{left}" x2="{width - right}" y1="{py(1):.1f}" y2="{py(1):.1f}" stroke="{MUTED}" stroke-width="1.5"/>')
    group_width = (width - left - right) / len(groups)
    for g, (label, mode, setting, baseline) in enumerate(groups):
        gx = left + g * group_width
        body.append(text(gx + group_width / 2, height - bottom + 40, label, 13, INK, "middle", "bold"))
        for v, view in enumerate(views):
            x = gx + group_width * (v + 0.75) / (len(views) + 0.5)
            m, lo, hi = geometric(per_model(curves_run, mode, setting, baseline, {view}).values())
            body.extend(vbar(x, lo, hi, y0, y1, py, COLOR["avif"]))
            body.append(marker("avif", x, py(m)))
            body.append(text(x, height - bottom + 18, {"full": "whole", "near": "3×", "back-near": "3× back", "close": "6×"}[view], 11, INK2, "middle"))
            body.append(text(x + 8, py(m) + 4, f"{m:.2f}", 11, INK2))
    write(name, width, height, body)


os.makedirs(OUT, exist_ok=True)
run_curves, _ = load(RUN)
sweep_curves, sweep_times = load(SWEEP)
slowest = {c: str(max(int(e) for (_, _, m, cc, e) in sweep_curves if cc == c)) for c in ("webp", "avif")}
own = effort_chart("effort-own.png", "Bytes saved by each encoder effort, 2k images",
                   "Bytes at equal rendered SSIMULACRA2, relative to the same codec's slowest effort.\nGeometric mean ×/÷ geometric SD over 16 models; encode times over images.",
                   sweep_curves, sweep_times, lambda c: (c, slowest[c]), threshold=1.05, y_max=1.7)
cross = effort_chart("effort-webp6.png", "Size and speed of WebP and AVIF efforts, 2k images",
                     "Bytes at equal rendered SSIMULACRA2, relative to WebP effort 6; below 1, smaller files.\nGeometric mean ×/÷ geometric SD over 16 models; encode times over images.",
                     sweep_curves, sweep_times, lambda c: ("webp", "6"))
views_chart("views.png", run_curves)
for label, points in (("own slowest", own), ("WebP 6", cross)):
    print(f"\n{label}: setting, time geometric mean [÷GSD, ×GSD], bytes ratio geometric mean [÷GSD, ×GSD]")
    for (codec, effort), ((tm, tlo, thi), (rm, rlo, rhi)) in sorted(points.items()):
        print(f"{codec} {effort}\t{tm:.2f} s [{tlo:.2f}, {thi:.2f}]\t{rm:.3f} [{rlo:.3f}, {rhi:.3f}]")
