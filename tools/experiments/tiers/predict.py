#!/usr/bin/env python3
"""Step 3's predictions of the two-tier budget experiment, written before any render:

    python3 -I tools/experiments/tiers/predict.py

Reads build/exp-tiers/models/*/images.csv (measure.py) and build/exp-tiers/scan.csv, and writes
build/exp-tiers/predictions.csv and build/exp-tiers/images.csv (every measured image).

Oversampling factor of an image, by each candidate measure, with thresholds at the middle of the gaps the
calibration cases left (calibrate.sh: sharp and hard-JPEG sources against the same images upsampled 2×, with
noise or JPEG blocking, and 4×):
- PSNR: 2 when the masked ÷2 PSNR is at least PSNR2 dB, 4 when the ÷4 PSNR is also at least PSNR4 dB, else 1.
- Ratio: 2 when E0/E1 is under RATIO1, 4 when E1/E2 is also under RATIO2, else 1.
An image's needed side is its longest side over its factor. Images not measured (beyond measure.py's largest
16 by texels in use) get the measured ones' effective share of texels.

Effective GPU memory: each image at its needed side, as the budget counts it (ETC1S 0.5 byte a texel for color
slots, UASTC 1 byte for data, ×4/3 for mips), plus the geometry (16 bytes a vertex, 12 a face). The tier, from
it: high over HIGH MiB (the detail needs well over the budget, and the budget's side would cut it), low at LOW
MiB or less (a 128 MiB budget would keep all of it), standard between.
"""
import csv
import glob
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import BUDGET, LOW_BUDGET, MIB, WORK, budget_side, fitted, geometry_bytes, is_color, used_images

PSNR2, PSNR4 = 30.4, 35.7
RATIO1, RATIO2 = 0.80, 0.35
HIGH, LOW = 384, 128


def factor_psnr(row):
    if float(row["psnr2"]) < PSNR2:
        return 1
    return 4 if float(row["psnr4"]) >= PSNR4 else 2


def factor_ratio(row):
    if float(row["e0_e1"]) >= RATIO1:
        return 1
    return 4 if float(row["e1_e2"]) < RATIO2 else 2


def predict(label, report, rows):
    images = {i["index"]: i for i in used_images(report)}
    geometry = geometry_bytes(report)
    result = {"model": label}
    measured = [r for r in rows if r.get("psnr2")]
    used = lambda r: int(r["width"]) * int(r["height"]) * float(r["occupancy"])
    total_used = sum(used(r) for r in rows)
    result["images"] = len(rows)
    result["measured"] = len(measured)
    result["measured_use_share"] = round(sum(used(r) for r in measured) / total_used, 3) if total_used else 0
    for name, factor in (("psnr", factor_psnr), ("ratio", factor_ratio)):
        need_bytes = 0.0
        need_used = 0.0
        sides = []
        share_num = share_den = 0.0
        for r in measured:
            f = factor(r)
            image = images[int(r["index"])]
            w, h = image["width"], image["height"]
            side = max(w, h) // f
            rate = 0.5 if is_color(image) else 1.0
            need_bytes += fitted(w, h, side) * rate * 4 / 3
            need_used += used(r) / f**2
            share_num += fitted(w, h, side)
            share_den += w * h
            sides.append((used(r), side, f))
        share = share_num / share_den if share_den else 1
        for r in rows:
            if not r.get("psnr2"):
                image = images[int(r["index"])]
                rate = 0.5 if is_color(image) else 1.0
                need_bytes += image["width"] * image["height"] * share * rate * 4 / 3
                need_used += used(r) * share
        effective = (need_bytes + geometry) / MIB
        result[f"factor_{name}"] = round(math.sqrt(1 / share), 2) if share else ""
        result[f"effective_texels_used_m_{name}"] = round(need_used / 1e6, 1)
        result[f"effective_gpu_mib_{name}"] = round(effective, 1)
        result[f"tier_{name}"] = "high" if effective > HIGH else "low" if effective <= LOW else "standard"
        # The side a single --max-size would give the images: the median needed side by texels in use.
        sides.sort(key=lambda s: s[1])
        half, acc = sum(s[0] for s in sides) / 2, 0.0
        for u, side, _ in sides:
            acc += u
            if acc >= half:
                result[f"side_need_{name}"] = side
                break
    side_256, _ = budget_side(report, BUDGET)
    side_128, _ = budget_side(report, LOW_BUDGET, floor=512)
    result.update({"side_source": max(max(i["width"], i["height"]) for i in images.values()) if images else 0,
                   "side_256": side_256, "side_128": side_128,
                   "tier": result["tier_psnr"], "side_need": result.get("side_need_psnr", ""),
                   "rule": f"factor: psnr2>={PSNR2} (x2), psnr4>={PSNR4} (x4); tier: effective GPU >{HIGH} MiB high, "
                           f"<={LOW} MiB low"})
    return result


def main():
    predictions, all_images = [], []
    for path in sorted(glob.glob(os.path.join(WORK, "models", "*", "images.csv"))):
        label = os.path.basename(os.path.dirname(path))
        with open(path) as f:
            rows = list(csv.DictReader(f))
        with open(os.path.join(os.path.dirname(path), "inspect.json")) as f:
            report = json.load(f)["report"]
        # The geometry is the shared geometry's where render.py made it (scan-44m's is simplified): images keep
        # the input's indices, which import may change.
        geometry = os.path.join(os.path.dirname(path), "geometry.json")
        if os.path.exists(geometry):
            with open(geometry) as f:
                report = dict(report, geometry=json.load(f)["report"]["geometry"])
        if not rows:
            continue
        predictions.append(predict(label, report, rows))
        for r in rows:
            if r.get("psnr2"):
                all_images.append({**{k: r[k] for k in ("model", "index", "mimeType", "codec", "slots", "width",
                                                         "height", "occupancy", "psnr2", "psnr4", "e0_e1", "e1_e2",
                                                         "k0", "b8", "b4")},
                                   "factor_psnr": factor_psnr(r), "factor_ratio": factor_ratio(r)})
    for name, rows in (("predictions.csv", predictions), ("images.csv", all_images)):
        with open(os.path.join(WORK, name), "w", newline="") as f:
            out = csv.DictWriter(f, list(rows[0].keys()))
            out.writeheader()
            out.writerows(rows)
        print("wrote", os.path.join(WORK, name), len(rows), "rows", file=sys.stderr)


if __name__ == "__main__":
    main()
