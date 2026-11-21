"""Summarizes webp-avif-render.py's scores.csv (docs/experiments/webp-avif.md#rendered-models).

For each pair of encoder settings, model and view, at each WebP point: the AVIF score at the same
total image bytes (interpolated linearly in score against log bytes), and the AVIF bytes at the
same score. Prints the medians and quartiles over models, per view and over all views, then the
same per model (median over its views and WebP points).

    python3 -I tools/experiments/textures/webp-avif-render-summary.py WORK/scores.csv [WORK/encodes.csv]

MIN_SCORE=70 in the environment keeps only WebP points scoring at least 70 ("high quality").
"""
import csv
import math
import os
import statistics
import sys
from collections import defaultdict

MIN_SCORE = float(os.environ.get("MIN_SCORE", "-1000"))

PAIRS = [  # label, mode, (webp effort), (avif effort)
    ("full size, default effort", "full", "4", "4"),
    ("2k, effort 0", "fast", "0", "0"),
    ("2k, best under 1 s (WebP 6, AVIF 3)", "fast", "6", "3"),
    ("2k, WebP 6 against AVIF 0", "fast", "6", "0"),
]

curves = defaultdict(list)  # (model, view, mode, codec, effort) -> [(bytes, score)]
reference = {}
for r in csv.DictReader(open(sys.argv[1])):
    if r["codec"] == "reference":
        reference[(r["model"], r["view"])] = float(r["ssimulacra2"])
        continue
    curves[(r["model"], r["view"], r["mode"], r["codec"], r["effort"])].append((int(r["bytes"]), float(r["ssimulacra2"])))


def interpolate(points, x, xi, yi):
    """y at x along points sorted by their x (xi, yi: tuple indices; bytes are on a log scale)."""
    fx = (lambda v: math.log(v)) if xi == 0 else (lambda v: v)
    fy = (lambda v: math.log(v)) if yi == 0 else (lambda v: v)
    gy = (lambda v: math.exp(v)) if yi == 0 else (lambda v: v)
    pts = sorted(points, key=lambda p: p[xi])
    for p0, p1 in zip(pts, pts[1:]):
        if p0[xi] <= x <= p1[xi] and p1[xi] > p0[xi]:
            f = (fx(x) - fx(p0[xi])) / (fx(p1[xi]) - fx(p0[xi]))
            return gy(fy(p0[yi]) + f * (fy(p1[yi]) - fy(p0[yi])))
    return None


def quartiles(values):
    if not values:
        return "n/a"
    if len(values) < 4:
        return f"{statistics.median(values):+.1f} (n={len(values)})" if values else "n/a"
    q = statistics.quantiles(values, n=4)
    return f"{q[1]:+.1f} [{q[0]:+.1f}, {q[2]:+.1f}] (n={len(values)})"


def ratios(values):
    if not values:
        return "n/a"
    q = statistics.quantiles(values, n=4) if len(values) >= 4 else [statistics.median(values)] * 3
    return f"{q[1]:.2f} [{q[0]:.2f}, {q[2]:.2f}]"


models = sorted({k[0] for k in curves})
views = sorted({k[1] for k in curves}, key=["full", "near", "close", "back-near"].index)
for label, mode, we, ae in PAIRS:
    print(f"\n## {label}\n")
    print("view\tAVIF score - WebP score, same bytes: median [quartiles]\tAVIF bytes / WebP bytes, same score")
    per_model = defaultdict(lambda: ([], []))
    for view in views + ["all"]:
        deltas, sizes = [], []
        for model in models:
            for v in (views if view == "all" else [view]):
                webp = curves.get((model, v, mode, "webp", we), [])
                avif = curves.get((model, v, mode, "avif", ae), [])
                for b, s in (p for p in webp if p[1] >= MIN_SCORE):
                    a = interpolate(avif, b, 0, 1)
                    if a is not None:
                        deltas.append(a - s)
                        if view == "all":
                            per_model[model][0].append(a - s)
                    ab = interpolate(avif, s, 1, 0)
                    if ab is not None:
                        sizes.append(ab / b)
                        if view == "all":
                            per_model[model][1].append(ab / b)
        print(f"{view}\t{quartiles(deltas)}\t{ratios(sizes)} (n={len(sizes)})")
    print("\nmodel\tscore delta (median)\tsize ratio (median)\tWebP scores\tAVIF scores")
    for model in models:
        d, r = per_model[model]
        ws = sorted(s for v in views for _, s in curves.get((model, v, mode, "webp", we), []))
        av = sorted(s for v in views for _, s in curves.get((model, v, mode, "avif", ae), []))
        print(f"{model}\t{statistics.median(d) if d else float('nan'):+.1f}\t{statistics.median(r) if r else float('nan'):.2f}"
              f"\t{ws[0]:.0f}-{ws[-1]:.0f}\t{av[0]:.0f}-{av[-1]:.0f}")

print("\n## Reference: images scaled to 2k against the model's own, rendered\n")
for view in views:
    values = [s for (m, v), s in reference.items() if v == view]
    print(f"{view}\tmedian {statistics.median(values):.1f}, min {min(values):.1f}")

if len(sys.argv) > 2:
    print("\n## Encode seconds per 2k image (wall, 4 cores)\n")
    times = defaultdict(list)
    for r in csv.DictReader(open(sys.argv[2])):
        if r["mode"] == "fast":
            times[(r["codec"], r["effort"])].append(float(r["seconds"]))
    for (codec, effort), t in sorted(times.items()):
        print(f"{codec} effort {effort}\tmedian {statistics.median(t):.2f} s, max {max(t):.2f} s (n={len(t)})")
