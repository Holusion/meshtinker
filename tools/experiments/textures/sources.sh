#!/bin/bash
# Writes the benchmark sources as PNG (compression level 1), so that every encoder decodes the same input.
# usage: sources.sh <outdir>. Reads the local models in place (read-only) and DamagedHelmet from test/assets.
set -e; O=$1; M=/home/sebastien/Documents/3DModels; R="$(cd "$(dirname "$0")/../../.." && pwd)"; mkdir -p "$O"
cv() { magick "$1" -define png:compression-level=1 "$O/$2.png"; }
cv "$M/collection/output/40-chauvet-sdf403/baseColor_1.jpg" color2k          # 2048², scan color
cv "$M/collection/output/01-akhenaton-nefertiti/Akhenaton_Nefertiti.jpg" color4k   # 4000², scan color
cv "$M/collection/output/17-golem/golem_u0_v0_diffuse.jpg" color8k           # 8192², scan color
cv "$M/Notre Dame en Vaux/20240610_DIGITAGE_NOTRE_DAME_EN_VAUX_10M_normal.1003.jpg" normal8k
python3 -I "$(dirname "$0")/glb_images.py" "$R/test/assets/khronos/DamagedHelmet/glTF-Binary/DamagedHelmet.glb" "$O"
cv "$O/helmet-normalTexture.jpg" normal2k; cv "$O/helmet-metallicRoughnessTexture.jpg" orm2k; rm -f "$O"/helmet-*.jpg
