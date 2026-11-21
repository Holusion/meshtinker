"""WebP against AVIF on rendered models (docs/experiments/webp-avif.md#rendered-models).

For each model: encodes every image of the model with WebP and AVIF over a range of qualities,
renders the model with each set (render_variants.py, Blender EEVEE, four views), and scores each
render against a reference render with SSIMULACRA2. Two modes:

- "full": images at their own size, encoders at their default effort (4), against the model's
  own images.
- "fast": images scaled down to at most 2048 texels a side (vips resize, Lanczos 3), encoders at
  effort 0 and at the slowest effort under 1 s, against the model with the scaled images stored losslessly. Encode times are measured
  one image at a time, from the scaled PNG.

Read-only on the models; writes only under WORK.

    python3 tools/experiments/textures/webp-avif-render.py WORK SSIMULACRA2_RS [--sweep] MODEL.gltf...

--sweep runs the fast mode only, over encoder efforts (WebP 0, 2, 4, 6; AVIF 0 to 6), to find the
effort past which each encoder gains little.

Appends one row per model, variant and view to WORK/scores.csv, and one row per encoded image to
WORK/encodes.csv. A model already in scores.csv is skipped, so a run can be resumed.
"""
import csv
import json
import os
import resource
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
WORK, SSIMULACRA2 = os.path.abspath(sys.argv[1]), sys.argv[2]
MODELS = sys.argv[3:]
SWEEP = MODELS[:1] == ["--sweep"]
if SWEEP:
    MODELS = MODELS[1:]
# (codec, effort): qualities. Fast efforts: the fastest, and the slowest under 1 s for a 2k image
# on 4 cores (AVIF effort 3: 0.4 to 0.5 s; WebP effort 6: 0.65 s).
FULL = {("webp", 4): [50, 70, 85, 95], ("avif", 4): [35, 50, 65, 80]}
FAST = {("webp", 0): [50, 65, 80, 90], ("avif", 0): [35, 50, 65, 80],
        ("webp", 6): [50, 65, 80, 90], ("avif", 3): [35, 50, 65, 80]}
if SWEEP:
    FULL = {}
    FAST = {("webp", e): [50, 65, 80, 90] for e in (0, 2, 4, 6)} | {("avif", e): [35, 50, 65, 80] for e in range(7)}
VIEWS = ["full", "near", "close", "back-near"]


def vips(*args):
    subprocess.run(["vips", *map(str, args)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def save_options(codec, q, effort):
    if codec == "webp":
        return f"[Q={q},effort={effort},keep=none]"
    return f"[Q={q},compression=av1,effort={effort},keep=none]"


def encode(source, out, codec, q, effort):
    """Encodes `source` and decodes the result to `out`.png; returns (bytes, wall seconds, CPU seconds).
    CPU seconds are only meaningful when encodes run one at a time (the fast mode)."""
    encoded = f"{out}.{codec}"
    start, before = time.monotonic(), resource.getrusage(resource.RUSAGE_CHILDREN)
    vips("copy", source, encoded + save_options(codec, q, effort))
    seconds = time.monotonic() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime
    vips("copy", encoded, out + ".png[compression=1]")
    size = os.path.getsize(encoded)
    os.remove(encoded)
    return size, seconds, cpu


def prepared(render, crop):
    """The render cropped to the reference's model area, over mid gray, as a PNG path."""
    image = Image.open(render).convert("RGBA").crop(crop)
    flat = Image.new("RGBA", image.size, (128, 128, 128, 255))
    flat.alpha_composite(image)
    path = render[:-4] + ".flat.png"
    flat.convert("RGB").save(path, compress_level=1)
    return path


def score(reference, distorted):
    out = subprocess.run([SSIMULACRA2, "image", reference, distorted], check=True, capture_output=True, text=True).stdout
    return float(out.split()[-1])


done = set()
scores_path, encodes_path = os.path.join(WORK, "scores.csv"), os.path.join(WORK, "encodes.csv")
if os.path.exists(scores_path):
    done = {r["model"] for r in csv.DictReader(open(scores_path))}
else:
    with open(scores_path, "w") as f:
        f.write("model,mode,codec,effort,q,bytes,view,ssimulacra2\n")
    with open(encodes_path, "w") as f:
        f.write("model,mode,codec,effort,q,image,width,height,bytes,seconds,cpu_seconds\n")

for model in MODELS:
    name = os.path.basename(os.path.dirname(model))
    if name in done:
        continue
    folder = os.path.dirname(model)
    uris = [i["uri"] for i in json.load(open(model)).get("images", []) if "uri" in i]
    root = os.path.join(WORK, name)
    variants, renders = os.path.join(root, "variants"), os.path.join(root, "renders")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(variants)
    encodes = []  # (mode, codec, effort, q, uri, width, height, bytes, seconds, cpu_seconds)

    # Fast mode's reference: images scaled to at most 2048, stored losslessly.
    scaled = {}
    os.makedirs(os.path.join(variants, "fast-reference"))
    for uri in uris:
        source = os.path.join(folder, uri)
        width, height = Image.open(source).size
        target = os.path.join(variants, "fast-reference", os.path.basename(uri) + ".png")
        factor = min(1.0, 2048 / max(width, height))
        vips("resize", source, target + "[compression=1]", factor, "--kernel", "lanczos3")
        scaled[uri] = (target, Image.open(target).size)

    # Full-size encodes run in parallel: their times aren't reported.
    jobs = []
    with ThreadPoolExecutor(3) as pool:
        for (codec, effort), qualities in FULL.items():
            for q in qualities:
                directory = os.path.join(variants, f"full-{codec}-{effort}-{q}")
                os.makedirs(directory)
                for uri in uris:
                    source = os.path.join(folder, uri)
                    out = os.path.join(directory, os.path.basename(uri))
                    size = Image.open(source).size
                    jobs.append((("full", codec, effort, q, uri, *size), pool.submit(encode, source, out, codec, q, effort)))
        for key, job in jobs:
            encodes.append((*key, *job.result()))
    # Fast encodes one at a time, timed.
    for (codec, effort), qualities in FAST.items():
        for q in qualities:
            directory = os.path.join(variants, f"fast-{codec}-{effort}-{q}")
            os.makedirs(directory)
            for uri in uris:
                source, size = scaled[uri]
                out = os.path.join(directory, os.path.basename(uri))
                encodes.append(("fast", codec, effort, q, uri, *size, *encode(source, out, codec, q, effort)))

    subprocess.run(["blender", "-b", "-P", os.path.join(HERE, "render_variants.py"), "--", model, variants, renders],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    rows = []
    totals = {}
    for mode, codec, effort, q, uri, w, h, size, seconds, cpu in encodes:
        totals[(mode, codec, effort, q)] = totals.get((mode, codec, effort, q), 0) + size
    for view in VIEWS:
        # The model's outline is the same in every render of a view: crop all to the reference's.
        original, fast_reference = (os.path.join(renders, v, view + ".png") for v in ("original", "fast-reference"))
        crop = Image.open(original).getchannel("A").getbbox()
        ref, fast_ref = prepared(original, crop), prepared(fast_reference, crop)
        rows.append((name, "fast", "reference", "", "", sum(os.path.getsize(s) for s, _ in scaled.values()), view, score(ref, fast_ref)))
        for (mode, codec, effort, q), size in sorted(totals.items()):
            render = prepared(os.path.join(renders, f"{mode}-{codec}-{effort}-{q}", view + ".png"), crop)
            rows.append((name, mode, codec, effort, q, size, view, score(ref if mode == "full" else fast_ref, render)))
    with open(scores_path, "a") as f:
        csv.writer(f).writerows(rows)
    with open(encodes_path, "a") as f:
        csv.writer(f).writerows((name, *e) for e in encodes)
    shutil.rmtree(variants)
    for directory, _, files in os.walk(renders):
        for file in files:
            if file.endswith(".flat.png"):
                os.remove(os.path.join(directory, file))
    print(name, "done", flush=True)
