"""Summarizes webp-avif-render.py --sweep (docs/experiments/webp-avif.md#rendered-models): for each
encoder and effort, the encode time of a 2k image, and its bytes at equal rendered SSIMULACRA2
against its own codec's slowest effort and against WebP effort 6 (medians and quartiles over
models, views and quality points, interpolated as in webp-avif-render-summary.py).

    python3 -I tools/experiments/textures/webp-avif-sweep-summary.py WORK/scores.csv WORK/encodes.csv
"""
import csv
import math
import statistics
import sys
from collections import defaultdict

curves = defaultdict(list)  # (model, view, codec, effort) -> [(bytes, score)]
for r in csv.DictReader(open(sys.argv[1])):
    if r["mode"] == "fast" and r["codec"] != "reference":
        curves[(r["model"], r["view"], r["codec"], int(r["effort"]))].append((int(r["bytes"]), float(r["ssimulacra2"])))
times = defaultdict(lambda: ([], []))
for r in csv.DictReader(open(sys.argv[2])):
    if r["mode"] == "fast":
        times[(r["codec"], int(r["effort"]))][0].append(float(r["seconds"]))
        times[(r["codec"], int(r["effort"]))][1].append(float(r["cpu_seconds"]))


def bytes_at(points, score):
    pts = sorted(points, key=lambda p: p[1])
    for (b0, s0), (b1, s1) in zip(pts, pts[1:]):
        if s0 <= score <= s1 and s1 > s0:
            f = (score - s0) / (s1 - s0)
            return math.exp(math.log(b0) + f * (math.log(b1) - math.log(b0)))
    return None


def ratio(setting, baseline):
    """AVIF-or-WebP `setting` bytes over `baseline` bytes at the baseline's scores."""
    values = []
    for (model, view, codec, effort), points in curves.items():
        if (codec, effort) != baseline:
            continue
        other = curves.get((model, view, *setting), [])
        for b, s in points:
            x = bytes_at(other, s)
            if x is not None:
                values.append(x / b)
    if len(values) < 4:
        return "n/a"
    q = statistics.quantiles(values, n=4)
    return f"{q[1]:.3f} [{q[0]:.3f}, {q[2]:.3f}]"


settings = sorted({(k[2], k[3]) for k in curves})
slowest = {codec: max(e for c, e in settings if c == codec) for codec, _ in settings}
print("setting\twall s / 2k image (median, max)\tCPU s\tbytes vs own slowest, same score\tbytes vs WebP 6, same score")
for codec, effort in settings:
    wall, cpu = times[(codec, effort)]
    print(f"{codec} {effort}\t{statistics.median(wall):.2f}, {max(wall):.2f}\t{statistics.median(cpu):.2f}"
          f"\t{ratio((codec, effort), (codec, slowest[codec]))}\t{ratio((codec, effort), ('webp', 6))}")
