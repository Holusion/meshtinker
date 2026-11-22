#!/usr/bin/env python3
"""Serves eCorpus's viewer (Voyager's explorer) and a folder of models, on localhost only.

    python3 tools/viewer/voyager.py NAME=DIR... [--port 8791] [--voyager ../eCorpus/source/voyager/dist]

Each DIR is served read-only under /NAME/: open
http://127.0.0.1:8791/voyager-explorer-dev-offline.html?model=/NAME/path/to/model.glb. Voyager is the viewer eCorpus embeds, with its own three.js (KTX2, meshopt, Draco, AVIF through
GLTFLoader), so this is how a model looks in eCorpus without running its server. docs/validation.md says when
to use it.
"""
import argparse
import functools
import http.server
import os
import posixpath
import urllib.parse

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, root, mounts, **kwargs):
        self.root = root
        self.mounts = mounts
        super().__init__(*args, directory=root, **kwargs)

    def do_GET(self):
        # Directory listings stay off: only files are served.
        if os.path.isdir(self.translate_path(self.path)):
            self.send_error(404)
            return
        super().do_GET()

    def translate_path(self, path):
        path = posixpath.normpath(urllib.parse.unquote(urllib.parse.urlsplit(path).path))
        name = path.split("/")[1] if path.count("/") >= 1 else ""
        if name in self.mounts:
            root, rest = self.mounts[name], path[len(name) + 1:]
        else:
            root, rest = self.root, path
        full = os.path.realpath(os.path.join(root, rest.lstrip("/")))
        # Nothing outside the two folders (symlinks resolved).
        if full != root and not full.startswith(root + os.sep):
            return os.path.join(root, "__refused__")
        return full

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, format, *args):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mounts", nargs="+", help="NAME=DIR")
    parser.add_argument("--port", type=int, default=8791)
    parser.add_argument("--voyager", default=os.path.join(REPO, "..", "eCorpus", "source", "voyager"))
    args = parser.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    voyager = os.path.realpath(args.voyager)
    mounts = {"voyager": os.path.join(voyager, "dist"), "three": os.path.join(voyager, "node_modules", "three")}
    for mount in args.mounts:
        name, _, directory = mount.partition("=")
        mounts[name] = os.path.realpath(directory)
    handler = functools.partial(Handler, root=here, mounts=mounts)
    http.server.SimpleHTTPRequestHandler.extensions_map.update({".glb": "model/gltf-binary", ".gltf": "model/gltf+json",
                                                                ".ktx2": "image/ktx2", ".avif": "image/avif",
                                                                ".wasm": "application/wasm"})
    print(f"http://127.0.0.1:{args.port}/view.html?model=/NAME/<file>", flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler).serve_forever()


if __name__ == "__main__":
    main()
