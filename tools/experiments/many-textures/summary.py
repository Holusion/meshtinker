#!/usr/bin/env python3
"""Markdown tables from treatments.py's results.csv, for docs/experiments/many-textures.md:

    python3 -I tools/experiments/many-textures/summary.py MODEL...

One table per model: each treatment's bytes, textures, estimated GPU memory, texels in use, headroom, time, and
SSIMULACRA2 against B (the base, same geometry with the input's images) per view.
"""
import csv
import os
import sys

EXP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "build", "exp-texels")


def mb(value):
    return f"{int(value) / 1e6:.0f}"


def main():
    for model in sys.argv[1:]:
        with open(os.path.join(EXP, model, "results.csv")) as f:
            rows = list(csv.DictReader(f))
        views, table = [], {}
        for r in rows:
            if r["view"] not in views:
                views.append(r["view"])
            table.setdefault(r["treatment"], {"row": r, "scores": {}})["scores"][r["view"]] = r["ssimulacra2_base"]
        print(f"\n{model}\n")
        print("| Treatment | MB | Textures MB | GPU MiB | Texels in use (M) | Headroom | Time (s) | "
              + " | ".join(views) + " |")
        print("| --- " * (7 + len(views)) + "|")
        for name, entry in table.items():
            r = entry["row"]
            scores = [entry["scores"][v] for v in views]
            cells = [f"{float(s):.0f}" if s else "—" for s in scores]
            print(f"| {name} | {mb(r['bytes'])} | {mb(r['texture_bytes'])} | {float(r['gpu_mib']):.0f} | "
                  f"{float(r['texels_used_m']):.0f} | {float(r['headroom']):.2f} | {r['seconds'] or '—'} | "
                  + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
