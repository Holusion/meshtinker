"""Shared helpers of the two-tier budget experiment (docs/experiments/many-textures.md, "Two tiers").

Local models are found through tools/corpus.mjs and named by their public labels only (docs/validation.md,
"The corpus"): `local/collection/NN-...` is `collection-NN`, any other `local/LABEL` is `LABEL`.
"""
import json
import os
import re
import subprocess
import time
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
BUILD = os.path.join(REPO, "build")
WORK = os.path.join(BUILD, "exp-tiers")
# Frozen copies of the commands, so that a rebuild doesn't change them during a run.
BIN = os.environ.get("BIN", os.path.join(WORK, "bin"))
SSIMULACRA2 = os.path.join(BUILD, "tools", "bin", "ssimulacra2_rs")
MIB = 2**20

# The plan's budget (src/commands/optimize/plan.cpp): 256 MiB for textures and geometry, sides in steps of 256
# under the quality's cap, down to a floor of 1024; images of 512 and less are left alone unless the
# model doesn't fit at the floor without encoding them.
BUDGET = 256 * MIB
LOW_BUDGET = 128 * MIB
CAP = 8192  # Highest's cap, so that the budget, not High's 4096, sets the side
STEP = 256
FLOOR = 1024
SMALL = 512


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, file=sys.stderr, flush=True)


def corpus_models():
    """(label, path) of every local model in the corpus, each file once."""
    listing = subprocess.run(["node", os.path.join(REPO, "tools", "corpus.mjs"), "list", "--source", "local"],
                             capture_output=True, text=True, check=True).stdout
    seen, models = set(), []
    for line in listing.splitlines():
        model_id, path = line.split("\t", 1)
        match = re.match(r"local/collection/(\d+)-", model_id)
        label = f"collection-{match.group(1)}" if match else model_id[len("local/"):]
        real = os.path.realpath(path)
        if real in seen:
            continue
        seen.add(real)
        models.append((label, path))
    return models


def model_path(label):
    for name, path in corpus_models():
        if name == label:
            return path
    sys.exit(f"{label} is not in the corpus")


def tool(name, *args, json_out=None, timeout=None):
    """Runs meshtinker-NAME with --json, returns (report, seconds, exit code)."""
    start = time.time()
    result = subprocess.run([os.path.join(BIN, f"meshtinker-{name}"), *map(str, args), "--json"],
                            capture_output=True, text=True, timeout=timeout)
    seconds = time.time() - start
    report = json.loads(result.stdout) if result.stdout.strip() else {}
    if json_out:
        with open(json_out, "w") as f:
            json.dump({"seconds": seconds, "exitCode": result.returncode, "report": report}, f)
    if result.returncode != 0:
        log(f"{name} exited with {result.returncode}: {result.stderr[-400:]}")
    return report, seconds, result.returncode


def is_color(image):
    """Color slots get ETC1S (0.5 byte a texel), data slots UASTC (1 byte), as ktx encodes them."""
    return any(use["slot"] in ("baseColorTexture", "emissiveTexture", "diffuseTexture") for use in image["uses"])


def bytes_per_texel(image):
    """GPU bytes a texel of the image as it is: KTX2 by its codec, anything else RGBA8."""
    if image["mimeType"] == "image/ktx2":
        return 0.5 if image.get("codec") == "ETC1S" else 1.0
    return 4.0


def fitted(width, height, side):
    scale = side / max(width, height) if max(width, height) > side else 1.0
    return int(width * scale) * int(height * scale)


def geometry_bytes(report):
    g = report.get("geometry", {})
    return g.get("vertices", 0) * 16 + g.get("faces", 0) * 12


def used_images(report):
    return [i for i in report.get("images", []) if i.get("uses")]


def texture_bytes_at(images, side, min_size):
    """plan.cpp's textureGpuBytesAt with ktx: images over min_size (and KTX2 that the side shrinks) encoded at
    side, the others as they are; ×4/3 for mips."""
    total = 0.0
    for i in images:
        w, h = i["width"], i["height"]
        ktx2 = i["mimeType"] == "image/ktx2"
        rate = 0.5 if is_color(i) else 1.0
        if ktx2 and max(w, h) > side:
            total += fitted(w, h, side) * rate
        elif ktx2 or max(w, h) <= min_size:
            total += w * h * bytes_per_texel(i)
        else:
            total += fitted(w, h, side) * rate
    return total * 4 / 3


def budget_side(report, budget, floor=FLOOR):
    """The texture step's side and min size for a budget, as plan.cpp's textureSide and minSize compute them."""
    images = used_images(report)
    room = budget - geometry_bytes(report)
    small_ones = any(i["mimeType"] != "image/ktx2" and max(i["width"], i["height"]) <= SMALL for i in images)
    min_size = 0 if small_ones and texture_bytes_at(images, floor, SMALL) > room else SMALL
    side = CAP
    while side > floor and texture_bytes_at(images, side, min_size) > room:
        side = max(floor, side - STEP)
    return side, min_size
