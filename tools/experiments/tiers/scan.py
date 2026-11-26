#!/usr/bin/env python3
"""Step 1 of the two-tier budget experiment: `inspect --json` of every local model in the corpus.

    python3 -I tools/experiments/tiers/scan.py [LABEL...]

Writes build/exp-tiers/inspect/LABEL.json, then build/exp-tiers/scan.csv: images, texels, texels in use, GPU
memory as it is, as ETC1S at full size, and the sides the 256 MiB and 128 MiB budgets give.
"""
import concurrent.futures
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import (BUDGET, LOW_BUDGET, MIB, WORK, budget_side, bytes_per_texel, corpus_models, geometry_bytes, log,
                    texture_bytes_at, tool, used_images)


def inspect(label, path):
    out = os.path.join(WORK, "inspect", f"{label}.json")
    if not os.path.exists(out):
        log("inspect", label)
        tool("inspect", path, "--trusted", json_out=out, timeout=3600)
    with open(out) as f:
        return json.load(f)


def row(label, run):
    report = run["report"]
    if run["exitCode"] != 0:
        return {"model": label, "error": (report.get("error") or {}).get("code", run["exitCode"])}
    images = used_images(report)
    texels = sum(i["width"] * i["height"] for i in images)
    used = sum(i["width"] * i["height"] * (i.get("occupancy") or 0) for i in images)
    as_is = sum(i["width"] * i["height"] * bytes_per_texel(i) for i in images) * 4 / 3
    side, min_size = budget_side(report, BUDGET)
    low_side, low_min = budget_side(report, LOW_BUDGET, floor=512)
    return {
        "model": label, "error": "", "bytes": report["input"]["byteSize"], "faces": report["geometry"]["faces"],
        "images": len(images), "largest": max((max(i["width"], i["height"]) for i in images), default=0),
        "ktx2": sum(i["mimeType"] == "image/ktx2" for i in images),
        "texels_m": round(texels / 1e6, 1), "texels_used_m": round(used / 1e6, 1),
        "occupancy": round(used / texels, 3) if texels else "",
        "geometry_mib": round(geometry_bytes(report) / MIB, 1),
        "gpu_as_is_mib": round(as_is / MIB, 1),
        "gpu_etc1s_full_mib": round(texture_bytes_at(images, 1 << 20, 0) / MIB, 1),
        "side_256": side, "min_size_256": min_size,
        "gpu_256_mib": round((texture_bytes_at(images, side, min_size) + geometry_bytes(report)) / MIB, 1),
        "side_128": low_side, "min_size_128": low_min,
        "gpu_128_mib": round((texture_bytes_at(images, low_side, low_min) + geometry_bytes(report)) / MIB, 1),
        "seconds": round(run["seconds"], 1),
    }


def main():
    os.makedirs(os.path.join(WORK, "inspect"), exist_ok=True)
    models = [(l, p) for l, p in corpus_models() if not sys.argv[1:] or l in sys.argv[1:]]
    with concurrent.futures.ThreadPoolExecutor(3) as pool:
        runs = dict(zip([l for l, _ in models], pool.map(lambda m: inspect(*m), models)))
    rows = [row(label, runs[label]) for label, _ in models]
    keys = max(rows, key=len).keys()
    with open(os.path.join(WORK, "scan.csv"), "w", newline="") as f:
        out = csv.DictWriter(f, keys)
        out.writeheader()
        out.writerows(rows)
    log("wrote", os.path.join(WORK, "scan.csv"))


if __name__ == "__main__":
    main()
