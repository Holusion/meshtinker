#!/usr/bin/env python3
"""Step 3 of the two-tier budget experiment: the oversampling measures on a model's images.

    python3 -I tools/experiments/tiers/measure.py LABEL...

For each model (a corpus label, as scan.py names them): `inspect --occupancy-maps` into
build/exp-tiers/models/LABEL/maps/, then masked.sh (tools/experiments/many-textures/) on its largest images by
texels in use, at most MAX_IMAGES of them, each extracted (KTX2 decoded to RGBA8) into a scratch folder that is
removed once measured. Writes build/exp-tiers/models/LABEL/images.csv: one row per used image, with the measures
where it was measured.
"""
import csv
import json
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import REPO, WORK, log, model_path, tool, used_images

MAX_IMAGES = 16
MASKED = os.path.join(REPO, "tools", "experiments", "many-textures", "masked.sh")
FIELDS = ["occupancy_map", "psnr2", "psnr4", "e0", "e1", "e2", "e0_e1", "e1_e2", "k0", "b8", "b4"]


def glb_image(path, index, out):
    """Writes image INDEX of a GLB to OUT.ext without reading the rest of the file; returns the file."""
    with open(path, "rb") as f:
        header = f.read(20)
        length = struct.unpack_from("<I", header, 12)[0]
        document = json.loads(f.read(length))
        image = document["images"][index]
        view = document["bufferViews"][image["bufferView"]]
        ext = {"image/ktx2": "ktx2", "image/png": "png", "image/webp": "webp"}.get(image["mimeType"], "jpg")
        f.seek(20 + length + 8 + view.get("byteOffset", 0))
        name = f"{out}.{ext}"
        with open(name, "wb") as o:
            o.write(f.read(view["byteLength"]))
    return name


def extract(model, image, out):
    if model.endswith(".glb") and not image.get("file"):
        src = glb_image(model, image["index"], out)
    else:
        src = os.path.join(os.path.dirname(model), image["file"])
    if src.endswith(".ktx2"):
        png = f"{out}-rgba8.png"
        subprocess.run(["ktx", "extract", "--transcode", "rgba8", src, png], check=True, capture_output=True)
        if src.startswith(os.path.dirname(out)):
            os.remove(src)
        return png
    return src


def measure(label):
    work = os.path.join(WORK, "models", label)
    maps = os.path.join(work, "maps")
    free = shutil.disk_usage(WORK).free / 2**30
    log(label, f"{free:.0f} GB free")
    if free < 8:
        sys.exit("under 8 GB free: stopping")
    path = model_path(label)
    if not os.path.exists(f"{work}/inspect.json"):
        os.makedirs(maps, exist_ok=True)
        tool("inspect", path, "--trusted", "--occupancy-maps", maps, json_out=f"{work}/inspect.json", timeout=3600)
    with open(f"{work}/inspect.json") as f:
        report = json.load(f)["report"]
    images = used_images(report)
    by_use = sorted(images, key=lambda i: -(i["width"] * i["height"] * (i.get("occupancy") or 0)))
    chosen = {i["index"] for i in by_use[:MAX_IMAGES]}
    done = {}
    out_csv = f"{work}/images.csv"
    if os.path.exists(out_csv):
        with open(out_csv) as f:
            done = {int(r["index"]): r for r in csv.DictReader(f) if r["psnr2"]}
    scratch = os.path.join(work, "img")
    os.makedirs(scratch, exist_ok=True)
    rows = []
    for image in images:
        row = {"model": label, "index": image["index"], "mimeType": image["mimeType"], "codec": image.get("codec") or "",
               "slots": "+".join(sorted({u["slot"] for u in image["uses"]})), "width": image["width"],
               "height": image["height"], "occupancy": round(image.get("occupancy") or 0, 4),
               "measured": int(image["index"] in chosen)}
        if image["index"] in done:
            row.update({k: done[image["index"]][k] for k in FIELDS})
        elif image["index"] in chosen:
            mask = os.path.join(maps, f"occupancy-{image['index']}.png")
            src = extract(path, image, os.path.join(scratch, str(image["index"])))
            env = dict(os.environ, TMPDIR=os.path.join(WORK, "tmp"))
            line = subprocess.run(["sh", MASKED, mask, src, str(image["index"])], capture_output=True, text=True,
                                  env=env).stdout.strip().split(",")
            if src.startswith(scratch):
                os.remove(src)
            if len(line) == 12:
                row.update(dict(zip(FIELDS, line[1:])))
                log(label, "image", image["index"], line[2:4], line[7:9])
            else:
                log(label, "image", image["index"], "masked.sh failed")
        rows.append(row)
        with open(out_csv, "w", newline="") as f:
            out = csv.DictWriter(f, list(rows[0].keys()) + [k for k in FIELDS if k not in rows[0]])
            out.writeheader()
            out.writerows(rows)
    shutil.rmtree(scratch, ignore_errors=True)
    log(label, "measured", len(chosen), "of", len(images))


if __name__ == "__main__":
    for name in sys.argv[1:]:
        measure(name)
