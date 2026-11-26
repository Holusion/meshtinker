#!/usr/bin/env python3
"""Steps 4 and 5 of the two-tier budget experiment: one model's treatments, renders and scores.

    python3 -I tools/experiments/tiers/render.py LABEL [STEP...]

STEPs, in order by default: geometry, encode, measure, render, score, clean. Work goes to
build/exp-tiers/models/LABEL/, which tools/viewer/serve.py must serve as /exp/:

    python3 tools/viewer/serve.py exp=build/exp-tiers

- geometry: `import` of the model: the geometry every treatment shares. SIMPLIFY lists the models whose import
  is too heavy to render (scan-44m: 44 M faces), which get `simplify` at High's error on top, as optimize would.
- encode: `ktx --max-size` on the shared geometry (as treatments.py's T2), at
  - REF: the sources' own side (at most 8192), every image ETC1S or UASTC: full detail, the reference. A model
    whose images are all KTX2 already is its own reference.
  - B256 and B128: the sides the 256 MiB and 128 MiB budgets give (common.budget_side), where under REF's;
  - NEED: the side the measure says the images need (build/exp-tiers/predictions.csv), where it differs.
  The min size is the 256 MiB budget's for all of them, so that the same images are encoded in each.
- measure: `inspect --json` of each output: GPU memory as the budget counts it.
- render: views.json's views of the model (tools/experiments/many-textures/views.json, keyed by label), or the
  three full views alone when it has none yet, at 1200×900.
- score: SSIMULACRA2 of each render against REF's of the same view: models/LABEL/results.csv.
- clean: removes the outputs' models, keeping reports, renders and the CSV.
"""
import csv
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import (BUDGET, LOW_BUDGET, MIB, REPO, SSIMULACRA2, WORK, budget_side, bytes_per_texel, geometry_bytes,
                    log, model_path, texture_bytes_at, tool, used_images)

SIMPLIFY = {"scan-44m": 0.0001}
VIEWS = os.path.join(REPO, "tools", "experiments", "many-textures", "views.json")
FULL = {"full-30": "az=30&el=30", "full-150": "az=150&el=30", "full-270": "az=270&el=30"}
PORT = 8791


def geometry(label, work):
    if os.path.exists(f"{work}/geometry/model.gltf"):
        return
    source = model_path(label)
    os.makedirs(f"{work}/import", exist_ok=True)
    log(label, "import")
    tool("import", source, f"{work}/import/model.gltf", "--trusted", json_out=f"{work}/import.json")
    if label in SIMPLIFY:
        os.makedirs(f"{work}/geometry", exist_ok=True)
        log(label, "simplify")
        tool("simplify", f"{work}/import/model.gltf", f"{work}/geometry/model.gltf", "--error", SIMPLIFY[label],
             json_out=f"{work}/simplify.json")
        shutil.rmtree(f"{work}/import")
    else:
        os.rename(f"{work}/import", f"{work}/geometry")
    tool("inspect", f"{work}/geometry/model.gltf", json_out=f"{work}/geometry.json")


def prediction(label):
    path = os.path.join(WORK, "predictions.csv")
    if os.path.exists(path):
        with open(path) as f:
            for row in csv.DictReader(f):
                if row["model"] == label:
                    return row
    return {}


def treatments(label, work):
    """(name, side) of each treatment to encode, REF first; side None for a model that is its own reference."""
    with open(f"{work}/geometry.json") as f:
        report = json.load(f)["report"]
    images = used_images(report)
    largest = min(8192, max(max(i["width"], i["height"]) for i in images))
    all_ktx2 = all(i["mimeType"] == "image/ktx2" for i in images)
    side_256, min_size = budget_side(report, BUDGET)
    side_128, _ = budget_side(report, LOW_BUDGET, floor=512)
    found = [("REF", None if all_ktx2 else largest)]
    sides = {largest}
    for name, side in (("B256", side_256), ("B128", side_128), ("NEED", int(prediction(label).get("side_need") or 0))):
        if 0 < side < largest and side not in sides:
            found.append((name, side))
            sides.add(side)
    return found, min_size


def encode(label, work):
    found, min_size = treatments(label, work)
    for name, side in found:
        out = f"{work}/{name.lower()}/model.gltf"
        if side is None or os.path.exists(out):
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        log(label, "ktx", name, side)
        _, seconds, code = tool("ktx", f"{work}/geometry/model.gltf", out, "--quality", "High", "--max-size", side,
                                "--min-size", min_size, json_out=f"{work}/{name.lower()}.json")
        log(label, "ktx", name, f"{seconds:.0f} s", "exit", code)


def outputs(label, work):
    found, _ = treatments(label, work)
    result = []
    for name, side in found:
        path = f"{work}/geometry/model.gltf" if side is None else f"{work}/{name.lower()}/model.gltf"
        if os.path.exists(path):
            result.append((name, side, path, "/exp/" + os.path.relpath(path, WORK)))
    return result


def measure(label, work):
    rows = {}
    for name, side, path, _ in outputs(label, work):
        report, _, _ = tool("inspect", path, json_out=f"{work}/inspect-{name}.json")
        images = used_images(report)
        textures = sum(i["width"] * i["height"] * bytes_per_texel(i) for i in images) * 4 / 3
        rows[name] = {"side": side or max(max(i["width"], i["height"]) for i in images),
                      "gpu_mib": round((textures + geometry_bytes(report)) / MIB, 1),
                      "texture_mib": round(textures / MIB, 1),
                      "texels_m": round(sum(i["width"] * i["height"] for i in images) / 1e6, 1),
                      "texture_bytes": sum(i["byteSize"] for i in images)}
    with open(f"{work}/measures.json", "w") as f:
        json.dump(rows, f, indent=1)


def views(label):
    with open(VIEWS) as f:
        return json.load(f).get(label, FULL)


def render(label, work):
    os.makedirs(f"{work}/renders", exist_ok=True)
    for name, _, _, url in outputs(label, work):
        files = [(f"{work}/renders/{name}-{v}.png", q) for v, q in views(label).items()]
        missing = [(f, q) for f, q in files if not os.path.exists(f)]
        if not missing:
            continue
        first, query = missing[0]
        page = f"http://127.0.0.1:{PORT}/view.html?model={url}&info=0&{query}"
        extra = []
        for f, q in missing[1:]:
            extra += ["--view", f"{f}={q}"]
        start = time.time()
        try:
            result = subprocess.run(["node", os.path.join(REPO, "tools", "viewer", "screenshot.mjs"), first, page,
                                     *extra, "--size", "1200x900", "--wait", "900"], capture_output=True, text=True,
                                    timeout=3600)
            drawn = result.stdout.count('"rendered":true')
            log(label, "rendered", name, f"{time.time() - start:.0f} s", drawn, "of", len(missing))
            if drawn != len(missing):
                log(result.stdout[-600:], result.stderr[-300:])
        except subprocess.TimeoutExpired:
            log(label, "render timed out", name)


def ssimulacra2(reference, image):
    result = subprocess.run([SSIMULACRA2, "image", reference, image], capture_output=True, text=True)
    return result.stdout.strip().split()[-1] if result.returncode == 0 else ""


def score(label, work):
    with open(f"{work}/measures.json") as f:
        measures = json.load(f)
    with open(f"{work}/results.csv", "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["model", "treatment", "side", "gpu_mib", "texture_mib", "texels_m", "texture_bytes", "view",
                      "ssimulacra2", "render"])
        for name, row in measures.items():
            for v in views(label):
                image, reference = f"{work}/renders/{name}-{v}.png", f"{work}/renders/REF-{v}.png"
                value = ""
                if name != "REF" and os.path.exists(image) and os.path.exists(reference):
                    value = ssimulacra2(reference, image)
                out.writerow([label, name, *row.values(), v, value, os.path.relpath(image, WORK)])
    log(label, "wrote", f"{work}/results.csv")


def clean(label, work):
    for entry in os.listdir(work):
        path = os.path.join(work, entry)
        if os.path.isdir(path) and entry not in ("renders", "maps"):
            shutil.rmtree(path)


STEPS = {"geometry": geometry, "encode": encode, "measure": measure, "render": render, "score": score, "clean": clean}


def main():
    label, steps = sys.argv[1], sys.argv[2:] or list(STEPS)
    work = os.path.join(WORK, "models", label)
    os.makedirs(work, exist_ok=True)
    for step in steps:
        free = min(shutil.disk_usage(WORK).free, shutil.disk_usage("/home").free) / 2**30
        log(label, step, f"{free:.0f} GB free")
        if free < 8 and step in ("geometry", "encode"):
            raise SystemExit("under 8 GB free: stopping before writing outputs")
        STEPS[step](label, work)


if __name__ == "__main__":
    main()
