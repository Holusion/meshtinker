#!/usr/bin/env python3
"""T4 for many small images (docs/experiments/many-textures.md): every base color image packed whole into one page,
texture coordinates moved onto it, and every primitive of the model's one mesh merged into one draw:

    python3 tools/experiments/many-textures/atlas_rects.py MODEL.gltf OUT_DIR [--padding 4] [--page 4096]

MODEL.gltf is a model in meshtinker's normalized profile (float attributes, separate files), with one mesh whose
primitives each have a material with a base color image (PNG or JPEG), as `uv`'s output of the campus scan.
Images are packed in shelves, tallest first, into as many pages as PAGE texels a side would take (4096: phones'
WebGL often goes no further, and SwiftShader stops at 8192), all at the smallest side (a multiple of 256) that
holds them in that many, so that a sweep scales them alike, each image with PADDING texels of its own edges around it, so that filtering and the first mip levels don't read
a neighbour. Each page gets one material and one primitive, merged from those of its images. Texture coordinates must stay within [0, 1]: a repeating texture can't share a page. Needs Pillow.
Writes OUT_DIR/model.gltf, model.bin and page-N.png, and prints the pages' sizes and fill.
Blender's island packer (atlas.py) suits models with few large charts; the campus's UVs are one island per
triangle, which it shrinks to nothing.
"""
import argparse
import array
import json
import math
import os
import sys

from PIL import Image

FLOAT, UINT, USHORT, UBYTE = 5126, 5125, 5123, 5121
SIZES = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
CODES = {FLOAT: "f", UINT: "I", USHORT: "H", UBYTE: "B"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model")
    parser.add_argument("out")
    parser.add_argument("--padding", type=int, default=4)
    parser.add_argument("--page", type=int, default=4096)
    args = parser.parse_args()
    folder = os.path.dirname(os.path.abspath(args.model))
    with open(args.model) as f:
        gltf = json.load(f)
    buffers = [open(os.path.join(folder, b["uri"]), "rb").read() for b in gltf["buffers"]]

    def read(index):
        accessor = gltf["accessors"][index]
        view = gltf["bufferViews"][accessor["bufferView"]]
        code = CODES[accessor["componentType"]]
        n = SIZES[accessor["type"]]
        item = array.array(code).itemsize
        stride = view.get("byteStride", n * item)
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        data = buffers[view["buffer"]]
        if stride == n * item:
            values = array.array(code)
            values.frombytes(data[start:start + accessor["count"] * stride])
            return values, n
        values = array.array(code)
        for i in range(accessor["count"]):
            values.frombytes(data[start + i * stride:start + i * stride + n * item])
        return values, n

    if len(gltf["meshes"]) != 1:
        sys.exit("one mesh only: merging primitives of several meshes would lose their nodes' transforms")
    primitives = gltf["meshes"][0]["primitives"]

    # The image each primitive samples, and its size.
    def image_of(primitive):
        material = gltf["materials"][primitive["material"]]
        texture = gltf["textures"][material["pbrMetallicRoughness"]["baseColorTexture"]["index"]]
        return texture["source"]

    images = sorted({image_of(p) for p in primitives})
    sizes = {i: Image.open(os.path.join(folder, gltf["images"][i]["uri"])).size for i in images}
    pad = args.padding
    limit = args.page
    rects = sorted(images, key=lambda i: (-sizes[i][1], -sizes[i][0]))
    if any(max(w, h) + 2 * pad > limit for w, h in sizes.values()):
        sys.exit("an image is larger than a page")

    def shelves(order, side):
        """Places images in shelves on a page of `side` until one doesn't fit: (placed, places)."""
        places, x, y, shelf = {}, 0, 0, 0
        for i in order:
            w, h = sizes[i][0] + 2 * pad, sizes[i][1] + 2 * pad
            if x + w > side:
                x, y, shelf = 0, y + shelf, 0
            if y + h > side:
                return places
            places[i] = (x, y)
            x, shelf = x + w, max(shelf, h)
        return places

    # Pages of one side for all, so that `ktx --max-size` scales them alike: as many as pages of `limit` take,
    # at the smallest side (a multiple of 256) that still holds the images in that many.
    def paginate(side):
        pages, left = [], rects
        while left:
            places = shelves(left, side)
            pages.append((side, places))
            left = [i for i in left if i not in places]
        return pages

    pages = paginate(limit)
    side = limit
    while side > 256 and len(paginate(side - 256)) == len(pages):
        side -= 256
    pages = paginate(side)
    page_of = {i: n for n, (_, places) in enumerate(pages) for i in places}

    os.makedirs(args.out, exist_ok=True)
    for n, (side, places) in enumerate(pages):
        page = Image.new("RGB", (side, side))
        for i, (x, y) in places.items():
            w, h = sizes[i]
            source = Image.open(os.path.join(folder, gltf["images"][i]["uri"])).convert("RGB")
            page.paste(source.resize((w + 2 * pad, h + 2 * pad), Image.Resampling.BILINEAR), (x, y))
            page.paste(source, (x + pad, y + pad))
        page.save(os.path.join(args.out, f"page-{n}.png"), compress_level=1)

    has_normals = all("NORMAL" in p["attributes"] for p in primitives)
    if not has_normals:
        sys.exit("every primitive needs normals: run `normals` first")
    merged = [{"positions": array.array("f"), "normals": array.array("f"), "uvs": array.array("f"),
               "indices": array.array("I")} for _ in pages]
    outside = 0
    for p in primitives:
        image = image_of(p)
        side, places = pages[page_of[image]]
        out = merged[page_of[image]]
        base = len(out["positions"]) // 3
        position, _ = read(p["attributes"]["POSITION"])
        out["positions"].extend(position)
        out["normals"].extend(read(p["attributes"]["NORMAL"])[0])
        uv, _ = read(p["attributes"]["TEXCOORD_0"])
        x, y = places[image]
        w, h = sizes[image]
        for k in range(0, len(uv), 2):
            u, v = uv[k], uv[k + 1]
            if not (-1e-4 <= u <= 1 + 1e-4 and -1e-4 <= v <= 1 + 1e-4):
                outside += 1
            out["uvs"].extend(((x + pad + u * w) / side, (y + pad + v * h) / side))
        if "indices" in p:
            out["indices"].extend(base + i for i in read(p["indices"])[0])
        else:
            out["indices"].extend(range(base, base + len(position) // 3))

    views, accessors, mesh_primitives, materials, offset = [], [], [], [], 0
    blobs = []
    template = gltf["materials"][primitives[0]["material"]]
    for n, out in enumerate(merged):
        count = len(out["positions"]) // 3
        first = len(accessors)
        for key, kind, component in (("positions", "VEC3", FLOAT), ("normals", "VEC3", FLOAT),
                                     ("uvs", "VEC2", FLOAT), ("indices", "SCALAR", UINT)):
            blob = out[key].tobytes()
            views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(blob)})
            blobs.append(blob)
            offset += (len(blob) + 3) // 4 * 4
            accessor = {"bufferView": len(views) - 1, "componentType": component,
                        "count": len(out[key]) // SIZES[kind], "type": kind}
            if key == "positions":
                accessor["min"] = [min(out[key][c::3]) for c in range(3)]
                accessor["max"] = [max(out[key][c::3]) for c in range(3)]
            accessors.append(accessor)
        mesh_primitives.append({"attributes": {"POSITION": first, "NORMAL": first + 1, "TEXCOORD_0": first + 2},
                                "indices": first + 3, "material": n})
        material = dict(template, name=f"atlas-{n}")
        material["pbrMetallicRoughness"] = dict(template["pbrMetallicRoughness"], baseColorTexture={"index": n})
        materials.append(material)
    with open(os.path.join(args.out, "model.bin"), "wb") as f:
        for blob in blobs:
            f.write(blob)
            f.write(b"\0" * ((4 - len(blob) % 4) % 4))
    result = {
        "asset": gltf["asset"], "scene": 0, "scenes": gltf["scenes"], "nodes": gltf["nodes"],
        "meshes": [{"name": gltf["meshes"][0].get("name", "mesh"), "primitives": mesh_primitives}],
        "materials": materials,
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}],
        "textures": [{"source": n, "sampler": 0} for n in range(len(pages))],
        "images": [{"uri": f"page-{n}.png", "mimeType": "image/png"} for n in range(len(pages))],
        "buffers": [{"uri": "model.bin", "byteLength": offset}],
        "bufferViews": views,
        "accessors": accessors,
    }
    with open(os.path.join(args.out, "model.gltf"), "w") as f:
        json.dump(result, f)
    used = sum(sizes[i][0] * sizes[i][1] for i in images)
    total = sum(side * side for side, _ in pages)
    print(f"{len(pages)} pages ({', '.join(str(side) for side, _ in pages)}), {len(images)} images, "
          f"{used / total:.3f} of them image texels, {len(primitives)} primitives into {len(pages)}, "
          f"{outside} texture coordinates outside [0, 1]")


if __name__ == "__main__":
    main()
