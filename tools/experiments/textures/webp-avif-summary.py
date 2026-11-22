"""Summarizes webp-avif.sh's CSV (docs/experiments/webp-avif.md): for each image and WebP quality,
the size of an AVIF of the same PSNR (and SSIM), interpolated on log(bytes) between AVIF rows of
the same effort, and the encode and decode CPU times on either side.

    python3 -I tools/experiments/textures/webp-avif-summary.py webp-avif.csv
"""
import csv
import math
import sys
from collections import defaultdict

rows = defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    rows[r["image"]].append(r)


def curve(image_rows, fmt, suffix=""):
    """The default-effort quality sweep of one format: configs "q<N>" followed by `suffix`."""
    return [r for r in image_rows if r["format"] == fmt and r["config"].startswith("q")
            and r["config"].split(" ", 1)[0][1:].isdigit() and r["config"][r["config"].index(" ") if " " in r["config"] else len(r["config"]):] == suffix]


def bytes_at(points, key, target):
    """log-linear interpolation of bytes at metric `target`, None outside the sweep."""
    pts = sorted((float(r[key]), math.log(int(r["bytes"])), r) for r in points if r[key])
    for (m0, b0, r0), (m1, b1, r1) in zip(pts, pts[1:]):
        if m0 <= target <= m1 and m1 > m0:
            f = (target - m0) / (m1 - m0)
            return math.exp(b0 + f * (b1 - b0)), float(r0["encode_cpu_s"]) + f * (float(r1["encode_cpu_s"]) - float(r0["encode_cpu_s"])), \
                float(r0["decode_cpu_s"]) + f * (float(r1["decode_cpu_s"]) - float(r0["decode_cpu_s"]))
    return None


print("image\twebp\tpsnr\tavif_444_same_psnr\twebp_bytes\tavif_bytes_same_psnr\tratio\tavif_bytes_same_ssim\tratio_ssim\twebp_enc_cpu\tavif_enc_cpu\twebp_dec_cpu\tavif_dec_cpu\tpng_dec_cpu")
for image, image_rows in rows.items():
    png = next(r for r in image_rows if r["format"] == "png")
    avif = curve(image_rows, "avif")
    avif444 = curve(image_rows, "avif", " 444")
    for w in curve(image_rows, "webp"):
        if not w["psnr"]:
            continue
        p = bytes_at(avif, "psnr", float(w["psnr"]))
        s = bytes_at(avif, "ssim", float(w["ssim"]))
        wb = int(w["bytes"])
        f = bytes_at(avif444, "psnr", float(w["psnr"]))
        cells = [image, w["config"], w["psnr"][:5], f"{f[0] / wb:.2f}" if f else "", str(wb)]
        cells += [f"{p[0]:.0f}", f"{p[0] / wb:.2f}"] if p else ["", ""]
        cells += [f"{s[0]:.0f}", f"{s[0] / wb:.2f}"] if s else ["", ""]
        cells += [w["encode_cpu_s"], f"{p[1]:.2f}" if p else "", w["decode_cpu_s"], f"{p[2]:.3f}" if p else "", png["decode_cpu_s"]]
        print("\t".join(cells))
