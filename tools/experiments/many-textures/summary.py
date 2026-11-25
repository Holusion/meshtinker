#!/usr/bin/env python3
"""Markdown tables from treatments.py's results.csv, for docs/experiments/many-textures.md:

    python3 -I tools/experiments/many-textures/summary.py MODEL[:REFERENCE]...

One table per model: each treatment's bytes, textures, estimated GPU memory, texels in use, headroom, time, and
SSIMULACRA2 against B (the base, same geometry with the input's images) per view, or against REFERENCE's renders
(a treatment's label), scored here. A JPEG source rendered beside a KTX2 output differs at every side, since
three.js makes the JPEG's mipmaps on the GPU and basisu makes KTX2's in linear light: against the KTX2 output
that kept every texel (Q1-4096 for the cave, T2-512 for the campus), the scores show the side's effect alone.
"""
import csv
import os
import subprocess
import sys

EXP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "build", "exp-texels")
SSIMULACRA2 = os.path.join(EXP, "..", "tools", "bin", "ssimulacra2_rs")


def score(reference, render):
    result = subprocess.run([SSIMULACRA2, "image", reference, render], capture_output=True, text=True)
    return result.stdout.strip().split()[-1] if result.returncode == 0 else ""


def mb(value):
    return f"{int(value) / 1e6:.0f}"


def main():
    for argument in sys.argv[1:]:
        model, _, reference = argument.partition(":")
        with open(os.path.join(EXP, model, "results.csv")) as f:
            rows = list(csv.DictReader(f))
        views, table = [], {}
        for r in rows:
            if r["view"] not in views:
                views.append(r["view"])
            value = r["ssimulacra2_base"]
            if reference:
                renders = os.path.join(EXP, model, "renders")
                value = "" if r["treatment"] in ("T0", "B", reference) else score(
                    os.path.join(renders, f"{reference}-{r['view']}.png"),
                    os.path.join(renders, f"{r['treatment']}-{r['view']}.png"))
            table.setdefault(r["treatment"], {"row": r, "scores": {}})["scores"][r["view"]] = value
        print(f"\n{model}, against {reference or 'B'}\n")
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
