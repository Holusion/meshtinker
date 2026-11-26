#!/usr/bin/env python3
"""Step 5 of the two-tier budget experiment: labelled pairs of renders for a person to judge by eye.

    python3 -I tools/experiments/tiers/sidebyside.py LABEL...

For each model, at its zoom-4 view and one full view: the 256 MiB budget's side beside full detail
(B256 | REF), the 128 MiB budget's beside the 256's (B128 | B256, or B128 | REF where the budget keeps full
detail), and the measure's needed side beside full detail (NEED | REF). Each half is labelled with the model, its
predicted tier, the side and the GPU memory. Writes build/exp-tiers/sidebyside/LABEL-VIEW-LEFT-RIGHT.jpg and
appends the pairs to build/exp-tiers/sidebyside/pairs.csv, which index.md is written from.
"""
import csv
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import WORK

OUT = os.path.join(WORK, "sidebyside")


def labelled(src, text, out):
    subprocess.run(["vips", "text", out + ".t.v", text, "--dpi", "150", "--font", "sans"], check=True)
    # White text on a dark strip above the render.
    subprocess.run(["vips", "embed", out + ".t.v", out + ".e.v", "12", "8", "1200", "44"], check=True)
    subprocess.run(["vips", "bandjoin", f"{out}.e.v {out}.e.v {out}.e.v", out + ".l.v"], check=True)
    subprocess.run(["vips", "extract_band", src, out + ".r.v", "0", "--n", "3"], check=True)
    subprocess.run(["vips", "arrayjoin", f"{out}.l.v {out}.r.v", out, "--across", "1"], check=True)
    for ext in (".t.v", ".e.v", ".l.v", ".r.v"):
        os.remove(out + ext)


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(WORK, "predictions.csv")) as f:
        predictions = {r["model"]: r for r in csv.DictReader(f)}
    rows = []
    for label in sys.argv[1:]:
        work = os.path.join(WORK, "models", label)
        with open(f"{work}/measures.json") as f:
            measures = json.load(f)
        scores = {}
        with open(f"{work}/results.csv") as f:
            for r in csv.DictReader(f):
                scores[(r["treatment"], r["view"])] = r["ssimulacra2"]
        views = sorted({v for _, v in scores})
        chosen = [v for v in views if v.endswith("z4")][:1] + [v for v in views if v.startswith("full")][:1]
        tier = predictions.get(label, {}).get("tier", "?")
        right_of_128 = "B256" if "B256" in measures else "REF"
        pairs = [("B256", "REF"), ("B128", right_of_128), ("NEED", "REF")]
        for view in chosen:
            for left, right in pairs:
                if left not in measures or right not in measures:
                    continue
                halves = []
                for name in (left, right):
                    m = measures[name]
                    text = f"{label}  predicted {tier}  {name}: side {m['side']}, {m['gpu_mib']:.0f} MiB"
                    out = os.path.join(OUT, f".{label}-{view}-{name}.v")
                    labelled(os.path.join(work, "renders", f"{name}-{view}.png"), text, out)
                    halves.append(out)
                pair = os.path.join(OUT, f"{label}-{view}-{left}-{right}.jpg")
                subprocess.run(["vips", "arrayjoin", " ".join(halves), pair + "[Q=90]", "--shim", "8"], check=True)
                for h in halves:
                    os.remove(h)
                rows.append({"model": label, "view": view, "left": left, "right": right,
                             "left_side": measures[left]["side"], "left_gpu_mib": measures[left]["gpu_mib"],
                             "right_side": measures[right]["side"], "right_gpu_mib": measures[right]["gpu_mib"],
                             "tier": tier, "ssimulacra2_left_vs_ref": scores.get((left, view), ""),
                             "file": os.path.basename(pair)})
    path = os.path.join(OUT, "pairs.csv")
    exists = os.path.exists(path)
    with open(path, "a", newline="") as f:
        out = csv.DictWriter(f, list(rows[0].keys()) if rows else ["model"])
        if not exists:
            out.writeheader()
        out.writerows(rows)


if __name__ == "__main__":
    main()
