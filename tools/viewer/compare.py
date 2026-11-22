#!/usr/bin/env python3
"""Side-by-side renders of models, one column per model and one row per view:

    python3 tools/viewer/compare.py OUT.png [--view AZ,EL,ZOOM[,TX,TY,TZ]]... [--size 800x600] LABEL=URLPATH...

URLPATH is a model's path on tools/viewer/serve.py's server (which must be running, on --port), such as
/khronos/DamagedHelmet/glTF-Binary/DamagedHelmet.glb. Each view is the camera's azimuth and elevation in degrees,
a zoom factor (1 frames the whole scene) and, optionally, the point looked at as fractions of the scene's bounds.
Renders go through view.html and screenshot.mjs; ImageMagick's montage assembles them.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("models", nargs="+", help="LABEL=URLPATH")
    parser.add_argument("--view", action="append", default=[])
    parser.add_argument("--size", default="800x600")
    parser.add_argument("--port", type=int, default=8791)
    parser.add_argument("--wait", type=int, default=300)
    args = parser.parse_args()
    views = args.view or ["30,15,1"]
    with tempfile.TemporaryDirectory() as tmp:
        tiles = []
        for v, view in enumerate(views):
            parts = view.split(",")
            for m, model in enumerate(args.models):
                label, _, path = model.partition("=")
                query = {"model": path, "az": parts[0], "el": parts[1], "zoom": parts[2]}
                if len(parts) == 6:
                    query["target"] = ",".join(parts[3:])
                url = f"http://127.0.0.1:{args.port}/view.html?" + urllib.parse.urlencode(query)
                tile = os.path.join(tmp, f"{v}-{m}.png")
                result = subprocess.run(["node", os.path.join(HERE, "screenshot.mjs"), tile, url, "--size", args.size,
                                         "--wait", str(args.wait)], capture_output=True, text=True)
                report = json.loads(result.stdout or "{}")
                if not report.get("rendered"):
                    print(f"{label} {view}: not rendered {report.get('messages')} {result.stderr[-300:]}", file=sys.stderr)
                elif report.get("messages"):
                    print(f"{label} {view}: {report['messages']}", file=sys.stderr)
                tiles += ["-label", f"{label}  ({view})", tile]
        subprocess.run(["montage", *tiles, "-tile", f"{len(args.models)}x{len(views)}", "-geometry", "+2+2",
                        "-pointsize", "16", "-background", "#202020", "-fill", "white", args.out], check=True)
    print(args.out)


if __name__ == "__main__":
    main()
