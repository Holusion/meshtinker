#!/usr/bin/env python3
"""The results table of the two-tier budget experiment, from render.py's results.csv of each rendered model:

    python3 -I tools/experiments/tiers/summary.py [--csv OUT.csv]

Per model and treatment: side, GPU MiB, and SSIMULACRA2 against full detail (REF): "full" is the mean of the
full views, "z2" and "z4" of the views whose name ends so (the cave's 30° and 15° views from inside).
"""
import csv
import glob
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import WORK

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    with open(os.path.join(WORK, "predictions.csv")) as f:
        predictions = {r["model"]: r for r in csv.DictReader(f)}
    with open(os.path.join(HERE, "sample.json")) as f:
        sample = [k for k in json.load(f) if not k.startswith("$") and k != "not rendered"]
    rows = []
    for label in sample:
        path = os.path.join(WORK, "models", label, "results.csv")
        if not os.path.exists(path):
            continue
        by_treatment = {}
        with open(path) as f:
            for r in csv.DictReader(f):
                by_treatment.setdefault(r["treatment"], []).append(r)
        p = predictions.get(label, {})
        for treatment, results in by_treatment.items():
            def mean(test):
                values = [float(r["ssimulacra2"]) for r in results if test(r["view"]) and r["ssimulacra2"]]
                return round(statistics.mean(values), 1) if values else ""
            rows.append({"model": label, "tier": p.get("tier", ""), "tier_ratio": p.get("tier_ratio", ""),
                         "effective_gpu_mib": p.get("effective_gpu_mib_psnr", ""),
                         "treatment": treatment, "side": results[0]["side"], "gpu_mib": results[0]["gpu_mib"],
                         "full": mean(lambda v: v.startswith("full")),
                         "z2": mean(lambda v: v.endswith("z2")), "z4": mean(lambda v: v.endswith("z4"))})
    if "--csv" in sys.argv:
        with open(sys.argv[sys.argv.index("--csv") + 1], "w", newline="") as f:
            out = csv.DictWriter(f, list(rows[0].keys()))
            out.writeheader()
            out.writerows(rows)
    print("| Model | Tier (PSNR / ratio) | Effective MiB | Treatment | Side | GPU MiB | full | z2 | z4 |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for r in rows:
        print(f"| {r['model']} | {r['tier']} / {r['tier_ratio']} | {r['effective_gpu_mib']} | {r['treatment']} | "
              f"{r['side']} | {r['gpu_mib']} | {r['full']} | {r['z2']} | {r['z4']} |")


if __name__ == "__main__":
    main()
