#!/bin/bash
# Writes the benchmark sources as PNG (compression level 1), so that every encoder decodes the same input.
# usage: sources.sh <outdir> <color2k> <color4k> <color8k> <normal8k>
# The four images are local scans' textures, read in place (read-only), given by path so that their names stay
# on the machine (docs/experiments/fast-textures.md lists which); DamagedHelmet's come from test/assets.
set -e
if [ $# -ne 5 ]; then sed -n 3p "$0" >&2; exit 2; fi
O=$1; R="$(cd "$(dirname "$0")/../../.." && pwd)"; mkdir -p "$O"
cv() { magick "$1" -define png:compression-level=1 "$O/$2.png"; }
cv "$2" color2k   # 2048², scan color
cv "$3" color4k   # 4000², scan color
cv "$4" color8k   # 8192², scan color
cv "$5" normal8k  # 8192², scan normal map
python3 -I "$(dirname "$0")/glb_images.py" "$R/test/assets/khronos/DamagedHelmet/glTF-Binary/DamagedHelmet.glb" "$O"
cv "$O/helmet-normalTexture.jpg" normal2k; cv "$O/helmet-metallicRoughnessTexture.jpg" orm2k; rm -f "$O"/helmet-*.jpg
