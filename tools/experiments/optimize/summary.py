#!/usr/bin/env python3
"""Summarizes tools/experiments/optimize/survey.sh's output, for docs/experiments/optimize-survey.md:

    python3 tools/experiments/optimize/summary.py OUT_DIR [CSV]

One row per model: what the input is like (the traits optimize should react to), what optimize did, how long
each step took, and what the output costs to download and to hold on the GPU.

GPU memory is estimated, with mipmaps (x 4/3): PNG and JPEG upload as RGBA8 (4 bytes a texel); KTX2 ETC1S
transcodes to BC1 or ETC2 RGB (0.5 byte, 1 with alpha) and UASTC to BC7 or ASTC 4x4 (1 byte), as three.js's
KTX2Loader picks them on desktop and phones.
"""
import csv
import json
import os
import sys

MIPS = 4 / 3


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def summarize(out_dir, name):
    inspect = load(os.path.join(out_dir, f"{name}.inspect.json")) or {}
    result = load(os.path.join(out_dir, f"{name}.optimize.json")) or {}
    with open(os.path.join(out_dir, f"{name}.run")) as f:
        code, seconds = f.read().split()
    steps = {s["command"]: s for s in result.get("steps", [])}
    reports = {k: s.get("report") or {} for k, s in steps.items()}
    geometry = inspect.get("geometry", {})
    images = inspect.get("images", [])
    texels = sum((i.get("width") or 0) * (i.get("height") or 0) for i in images)
    sides = [max(i.get("width") or 0, i.get("height") or 0) for i in images]
    gpu_in = sum((i.get("width") or 0) * (i.get("height") or 0) * 4 * MIPS for i in images
                 if i.get("mimeType") in ("image/png", "image/jpeg", "image/webp", "image/avif"))
    gpu_in += sum((i.get("width") or 0) * (i.get("height") or 0) * 1 * MIPS for i in images
                  if i.get("mimeType") == "image/ktx2")
    ktx = reports.get("ktx", {})
    encoded = {e["source"]: e for e in ktx.get("images", [])}
    # Encoded images as ktx reports them (uv may have renamed and resized
    # them); images ktx left alone as they were.
    gpu_out = 0.0
    for e in encoded.values():
        per = 1.0 if e["codec"] == "UASTC" or e.get("alpha") else 0.5
        gpu_out += e["outputWidth"] * e["outputHeight"] * per * MIPS
    if ktx.get("totals", {}).get("skipped", 0) or not encoded:
        for i in images:
            if i.get("file") in encoded:
                continue
            w, h = i.get("width") or 0, i.get("height") or 0
            gpu_out += w * h * (1 if i.get("mimeType") == "image/ktx2" else 4) * MIPS
    simplify = reports.get("simplify", {})
    weld = reports.get("weld", {})
    normals = reports.get("normals", {}).get("primitives", {})
    uv = reports.get("uv", {}).get("totals", {})
    faces = geometry.get("faces", 0)
    vertices = geometry.get("vertices", 0)
    out_bytes = result.get("output", {}).get("byteSize")
    row = {
        "model": name,
        "exit": int(code),
        "seconds": round(float(seconds), 1),
        "input_mb": round(inspect.get("input", {}).get("byteSize", 0) / 1e6, 2),
        "output_mb": round(out_bytes / 1e6, 2) if out_bytes else "",
        "primitives": geometry.get("primitives", 0),
        "faces": faces,
        "vertices_per_face": round(vertices / faces, 2) if faces else "",
        "encoding": ",".join(k for k, v in geometry.get("primitivesByEncoding", {}).items() if v),
        "normals_written": (normals.get("added", 0) + normals.get("smoothed", 0)) if normals else "",
        "welded_vertices": (f"{weld['input']['vertices']}->{weld['output']['vertices']}"
                            if weld.get("primitives", {}).get("welded") else ""),
        "faces_out": simplify.get("output", {}).get("faces", ""),
        "images": len(images),
        "largest_side": max(sides) if sides else 0,
        "mtexels": round(texels / 1e6, 1),
        "occupancy": round(inspect.get("texels", {}).get("occupancy") or 0, 2) if images else "",
        "uv_repacked": uv.get("repacked", "") if uv else "",
        "uv_texels": (f"{uv['texels']['before'] / 1e6:.1f}->{uv['texels']['after'] / 1e6:.1f}"
                      if uv.get("texels") else ""),
        "gpu_in_mb": round(gpu_in / 2**20),
        "gpu_out_mb": round(gpu_out / 2**20) if ktx else "",
    }
    for step in ("import", "normals", "weld", "dedup", "simplify", "uv", "ktx", "avif", "compress", "export"):
        row[f"t_{step}"] = round(steps[step]["seconds"], 1) if step in steps else ""
    row["failed_step"] = next((s["command"] for s in result.get("steps", []) if s.get("exitCode") not in (0, None)), "")
    return row


def main():
    out_dir = sys.argv[1]
    names = sorted(f[:-4] for f in os.listdir(out_dir) if f.endswith(".run"))
    rows = [summarize(out_dir, n) for n in names]
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
    cols = ["model", "exit", "seconds", "input_mb", "output_mb", "faces", "faces_out", "vertices_per_face",
            "images", "largest_side", "mtexels", "occupancy", "gpu_in_mb", "gpu_out_mb", "t_simplify", "t_uv", "t_ktx"]
    print("\t".join(cols))
    for r in rows:
        print("\t".join(str(r[c])[:34] for c in cols))


if __name__ == "__main__":
    main()
