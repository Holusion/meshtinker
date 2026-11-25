#!/usr/bin/env python3
"""The treatments of docs/experiments/many-textures.md on one model, with their measurements and renders:

    python3 -I tools/experiments/many-textures/treatments.py MODEL [STEP...]

MODEL is a key of MODELS below. STEPs, in order by default: opt, base, t2, t3, q1, measure, render, score, clean.
Work goes to build/exp-texels/MODEL/ (on disk: a sweep writes several hundred MB), which tools/viewer/serve.py
must serve as /exp/ (and the local models' folders as /raw/ and /normalized/) for the renders:

    python3 tools/viewer/serve.py raw=.../collection/raw normalized=.../collection/normalized exp=build/exp-texels

- opt: `optimize` at High, slow, as the plan runs it today (--keep-tmp, so that its chain gives the base).
- base: the optimize chain's stage before uv and the texture step, exported as .gltf: the geometry every
  treatment shares.
- t2: `ktx` on the base at each side of the sweep. t3: `uv` on the base, then `ktx` at the model's side.
  t1: `ktx`, then `compress`, at the side the budget gives, without uv, where optimize runs uv.
- q1: where the sources aren't KTX2 (the cave), the second lossy round alone: `ktx` at 4096, as a careless
  first compression, then `ktx` at 2048 on it, against T2-2048 straight from the sources.
- t4: `ktx` on atlas_rects.py's pages (build/exp-texels/MODEL-t4/input-page and uv-page) at their side, ÷2, ÷4.
- measure: `inspect --json` of each output: bytes, texels, texels in use, GPU estimate, headroom.
- render: every view of views.json for each output and for the input (T0), at 1200×900 without the label:
  SwiftShader drew nothing at 1600×1200 once zoomed in on MAN80287's 103 4k textures.
- score: SSIMULACRA2 of each render against T0's of the same view, and against B's (ssimulacra2_base): B, the
  base, has the input's images on the shared geometry, so it isolates what the texture steps lose from what
  `normals` (smooth normals where the input had none or flat ones) and `simplify` change. Writes MODEL/results.csv.
- clean: removes the outputs' models, keeping JSON reports, renders and the CSV.
"""
import csv
import json
import math
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
BUILD = os.path.join(REPO, "build")
EXP = os.path.join(BUILD, "exp-texels")
SSIMULACRA2 = os.path.join(BUILD, "tools", "bin", "ssimulacra2_rs")
# The commands, copied from the build so that a rebuild doesn't change them during a run (BIN=build to use it).
BIN = os.environ.get("BIN", os.path.join(EXP, "bin"))
HERE = os.path.dirname(os.path.abspath(__file__))
LOCAL = "/home/sebastien/Documents/3DModels/collection"
SWEEP = [3072, 2048, 1536, 1024, 768, 512]

# Surface area (Blender, area.py) and bounding diagonal, for headroom: docs/experiments/many-textures.md.
MODELS = {
    "man80287": {"input": f"{LOCAL}/raw/MAN80287.glb", "url": "/raw/MAN80287.glb", "area": 20.9, "diagonal": 4.81,
                 "sweep": SWEEP, "min_size": 512, "t1": 1792, "t3": 1792},
    "sorcier": {"input": f"{LOCAL}/raw/Grotte_du_sorcier.glb", "url": "/raw/Grotte_du_sorcier.glb", "area": 600,
                "diagonal": 25.9, "sweep": SWEEP, "min_size": 512, "t1": None, "t3": 2560, "q1": True},
    "campus": {"input": f"{LOCAL}/normalized/32-campus-romainville/model.gltf",
               "url": "/normalized/32-campus-romainville/model.gltf", "area": 678, "diagonal": 26.2,
               "sweep": [512, 256, 128], "min_size": 0, "t1": None, "t3": None},
}


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, file=sys.stderr, flush=True)


def tool(name, *args, json_out=None):
    """Runs meshtinker-NAME with --json, returns (report, seconds)."""
    start = time.time()
    result = subprocess.run([os.path.join(BIN, f"meshtinker-{name}"), *map(str, args), "--json"],
                            capture_output=True, text=True)
    seconds = time.time() - start
    report = json.loads(result.stdout) if result.stdout.strip() else {}
    if json_out:
        with open(json_out, "w") as f:
            json.dump({"seconds": seconds, "exitCode": result.returncode, "report": report}, f)
    if result.returncode != 0:
        log(f"{name} exited with {result.returncode}: {result.stderr[-500:]}")
    return report, seconds


def opt(m, work):
    os.makedirs(f"{work}/opt", exist_ok=True)
    os.makedirs(f"{work}/ws", exist_ok=True)
    tool("optimize", m["input"], f"{work}/opt/model.glb", "--keep-tmp", "--tmp-dir", f"{work}/ws",
         json_out=f"{work}/opt.json")


def chain_dir(work):
    for root, dirs, files in os.walk(f"{work}/ws"):
        if "chain.json" in files:
            return root
    raise SystemExit("no chain in the optimize workspace: run the opt step")


def base(m, work):
    chain = chain_dir(work)
    stages = sorted(d for d in os.listdir(chain) if d[:1].isdigit())
    before = [d for d in stages if d.split("-")[1] in ("normals", "weld", "dedup", "simplify")]
    stage = int(before[-1].split("-")[0])
    log("base is stage", before[-1])
    os.makedirs(f"{work}/base", exist_ok=True)
    tool("export", chain, f"{work}/base/model.gltf", "--stage", stage, json_out=f"{work}/base.json")


def ktx(m, source, out, side):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    return tool("ktx", source, out, "--quality", "High", "--max-size", side, "--min-size", m["min_size"],
                json_out=os.path.join(os.path.dirname(out), "..", os.path.basename(os.path.dirname(out)) + ".json"))


def t2(m, work):
    for side in m["sweep"]:
        if not os.path.exists(f"{work}/t2-{side}/model.gltf"):
            log("t2", side)
            ktx(m, f"{work}/base/model.gltf", f"{work}/t2-{side}/model.gltf", side)
    if m["t1"] and not os.path.exists(f"{work}/t1/model.gltf"):
        log("t1", m["t1"])
        ktx(m, f"{work}/base/model.gltf", f"{work}/t1-ktx/model.gltf", m["t1"])
        os.makedirs(f"{work}/t1", exist_ok=True)
        tool("compress", f"{work}/t1-ktx/model.gltf", f"{work}/t1/model.gltf", json_out=f"{work}/t1-compress.json")


def t3(m, work):
    if not m["t3"] or os.path.exists(f"{work}/t3/model.gltf"):
        return
    log("t3 uv")
    os.makedirs(f"{work}/t3-uv", exist_ok=True)
    tool("uv", f"{work}/base/model.gltf", f"{work}/t3-uv/model.gltf", "--max-size", m["t3"],
         json_out=f"{work}/t3-uv.json")
    log("t3 ktx", m["t3"])
    ktx(m, f"{work}/t3-uv/model.gltf", f"{work}/t3/model.gltf", m["t3"])


def q1(m, work):
    if not m.get("q1"):
        return
    if not os.path.exists(f"{work}/q1-4096/model.gltf"):
        log("q1 4096")
        ktx(m, f"{work}/base/model.gltf", f"{work}/q1-4096/model.gltf", 4096)
    if not os.path.exists(f"{work}/q1-2048/model.gltf"):
        log("q1 2048 again")
        ktx(m, f"{work}/q1-4096/model.gltf", f"{work}/q1-2048/model.gltf", 2048)


def t4(m, work):
    """Atlases from atlas_rects.py in build/exp-texels/MODEL-t4/: input-page (the input's images) and uv-page
    (uv's), each swept at its side, ÷2 and ÷4."""
    for kind, prefix in (("input-page", "t4i"), ("uv-page", "t4u")):
        page = f"{work}-t4/{kind}/model.gltf"
        if not os.path.exists(page):
            log(f"no {kind}: run atlas_rects.py first")
            continue
        report, _ = tool("inspect", page)
        side = max(max(i["width"], i["height"]) for i in report.get("images", []))
        for s in (side, side // 2, side // 4):
            if not os.path.exists(f"{work}/{prefix}-{s}/model.gltf"):
                log(prefix, s)
                ktx(dict(m, min_size=0), page, f"{work}/{prefix}-{s}/model.gltf", s)


def outputs(m, work):
    """Treatment name, the model file to measure and render, and its server path."""
    found = [("T0", m["input"], m["url"])]
    name = os.path.basename(work)
    # B, the base: the geometry steps with the input's images, the reference for what the texture steps lose.
    candidates = [("B", "base/model.gltf"), ("T1-opt", "opt/model.glb")] + [(f"T2-{s}", f"t2-{s}/model.gltf") for s in m["sweep"]]
    candidates += [("T1-nouv", "t1/model.gltf"), ("T3", "t3/model.gltf"), ("Q1-4096", "q1-4096/model.gltf"),
                   ("Q1-2048", "q1-2048/model.gltf")]
    for prefix in ("t4i", "t4u"):
        candidates += sorted(((f"T4{prefix[2]}-{d[4:]}", f"{d}/model.gltf") for d in os.listdir(work)
                              if d.startswith(prefix + "-") and os.path.isdir(f"{work}/{d}")), key=lambda c: -int(c[0].split("-")[1]))
    for label, path in candidates:
        if os.path.exists(f"{work}/{path}"):
            found.append((label, f"{work}/{path}", f"/exp/{name}/{path}"))
    return found


def folder_bytes(path):
    if path.endswith(".glb"):
        return os.path.getsize(path)
    folder = os.path.dirname(path)
    return sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder))


def measure(m, work):
    rows = {}
    for label, path, _ in outputs(m, work):
        report, seconds = tool("inspect", path, "--trusted", json_out=f"{work}/inspect-{label}.json")
        texels = used = gpu = texture_bytes = 0.0
        for image in report.get("images", []):
            if not image.get("uses"):
                continue
            t = image["width"] * image["height"]
            texels += t
            used += (image.get("occupancy") or 0) * t
            bpt = 4 if image["mimeType"] != "image/ktx2" else (0.5 if image.get("codec") == "ETC1S" else 1)
            gpu += t * bpt * 4 / 3
            texture_bytes += image["byteSize"]
        geometry = report.get("geometry", {})
        gpu += geometry.get("vertices", 0) * 16 + geometry.get("faces", 0) * 12
        materials = len({use["material"] for image in report.get("images", []) for use in image.get("uses", [])})
        rows[label] = {
            "bytes": folder_bytes(path), "texture_bytes": int(texture_bytes), "gpu_mib": round(gpu / 2**20, 1),
            "texels_m": round(texels / 1e6, 1), "texels_used_m": round(used / 1e6, 1),
            "headroom": round(math.sqrt(used / m["area"]) * m["diagonal"] / 4096, 2) if used else 0,
            "primitives": geometry.get("primitives", 0), "faces": geometry.get("faces", 0), "materials": materials,
            "images": len(report.get("images", [])),
        }
        log("measured", label, rows[label])
    with open(f"{work}/measures.json", "w") as f:
        json.dump(rows, f, indent=1)


def views():
    with open(os.path.join(HERE, "views.json")) as f:
        return json.load(f)


def render(m, work):
    name = os.path.basename(work)
    model_views = views()[name]
    os.makedirs(f"{work}/renders", exist_ok=True)
    for label, _, url in outputs(m, work):
        files = [(f"{work}/renders/{label}-{v}.png", q) for v, q in model_views.items()]
        if all(os.path.exists(f) for f, _ in files):
            continue
        first, query = files[0]
        page = f"http://127.0.0.1:8791/view.html?model={url}&info=0&{query}"
        extra = []
        for f, q in files[1:]:
            extra += ["--view", f"{f}={q}"]
        start = time.time()
        result = subprocess.run(["node", os.path.join(REPO, "tools", "viewer", "screenshot.mjs"), first, page, *extra,
                                 "--size", "1200x900", "--wait", "900"], capture_output=True, text=True,
                                timeout=3600)
        log("rendered", label, f"{time.time() - start:.0f} s", result.stdout.count('"rendered":true'), "of", len(files))
        if result.stdout.count('"rendered":true') != len(files):
            log(result.stdout[-600:], result.stderr[-300:])


def score(m, work):
    name = os.path.basename(work)
    with open(f"{work}/measures.json") as f:
        measures = json.load(f)
    seconds = {}
    for label in measures:
        reports = {"T1-opt": "opt.json", "T1-nouv": "t1-ktx.json", "T3": "t3.json"}
        path = f"{work}/{reports.get(label, label.lower() + '.json')}"
        if os.path.exists(path):
            with open(path) as f:
                seconds[label] = round(json.load(f)["seconds"], 1)
    with open(f"{work}/results.csv", "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["model", "treatment", *next(iter(measures.values())).keys(), "seconds", "view", "ssimulacra2",
                      "ssimulacra2_base", "render", "reference"])
        for label, row in measures.items():
            for v in views()[name]:
                render_path = f"{work}/renders/{label}-{v}.png"
                reference = f"{work}/renders/T0-{v}.png"
                values = []
                for ref, skip in ((reference, "T0"), (f"{work}/renders/B-{v}.png", "B")):
                    value = ""
                    if label not in ("T0", skip) and os.path.exists(render_path) and os.path.exists(ref):
                        result = subprocess.run([SSIMULACRA2, "image", ref, render_path], capture_output=True,
                                                text=True)
                        value = result.stdout.strip().split()[-1] if result.returncode == 0 else ""
                    values.append(value)
                out.writerow([name, label, *row.values(), seconds.get(label, ""), v, *values,
                              os.path.relpath(render_path, EXP), os.path.relpath(reference, EXP)])
    log("wrote", f"{work}/results.csv")


def clean(m, work):
    for entry in os.listdir(work):
        path = os.path.join(work, entry)
        if os.path.isdir(path) and entry not in ("renders",):
            shutil.rmtree(path)


STEPS = {"opt": opt, "base": base, "t2": t2, "t3": t3, "q1": q1, "t4": t4, "measure": measure, "render": render, "score": score,
         "clean": clean}


def main():
    name, steps = sys.argv[1], sys.argv[2:] or list(STEPS)
    m = MODELS[name]
    work = os.path.join(EXP, name)
    os.makedirs(work, exist_ok=True)
    for step in steps:
        free = shutil.disk_usage(EXP).free / 2**30
        log(name, step, f"{free:.0f} GB free")
        if free < 8 and step in ("opt", "base", "t2", "t3", "q1", "t4"):
            raise SystemExit("under 8 GB free: stopping before writing outputs")
        STEPS[step](m, work)


if __name__ == "__main__":
    main()
