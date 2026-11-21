#!/usr/bin/env bash
# What `normals` does to real models, and to `weld` after it (docs/normals.md#results).
# For each model: weld it alone, then write normals and weld the result.
# One tab-separated line per model on stdout. Read-only on the models;
# everything is written under OUT.
#
#   tools/experiments/normals/measure.sh BUILD OUT [--crease-angle DEGREES] MODEL...
set -euo pipefail
BUILD=$1 OUT=$2; shift 2
EXTRA=()
if [ "${1:-}" = --crease-angle ]; then EXTRA=(--crease-angle "$2"); shift 2; fi
export LC_ALL=C
# run LABEL COMMAND ARGS...: prints "seconds peak_rss_kb", the JSON report in $OUT/LABEL.json.
run() {
  local label=$1; shift
  python3 -c '
import os, subprocess, sys, time
start = time.monotonic()
with open(sys.argv[1], "w") as out:
    process = subprocess.Popen(sys.argv[2:], stdout=out)
    _, status, usage = os.wait4(process.pid, 0)
if status != 0:
    sys.exit(f"{sys.argv[2]} failed: {status}")
print(f"{time.monotonic() - start:.2f} {usage.ru_maxrss}")
' "$OUT/$label.json" "$BUILD/meshtinker-$1" --json "${@:2}"
}
field() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))" "$OUT/$1.json" "$2"; }
printf 'model\tadded\tsmoothed\tkept\tvertices\twith_normals\tnormals_s\tnormals_rss_kb\twelded_alone\twelded_after_normals\n'
for model in "$@"; do
  name=$(echo "$model" | sed 's|.*/3DModels/||; s|[/ ]|_|g; s|\.gl[tb]f*$||')
  mkdir -p "$OUT/$name/alone" "$OUT/$name/normals" "$OUT/$name/welded"
  run "$name/weld-alone" weld "$model" "$OUT/$name/alone/model.gltf" > /dev/null
  read -r seconds rss < <(run "$name/normals" normals "${EXTRA[@]}" "$model" "$OUT/$name/normals/model.gltf")
  run "$name/weld-after" weld "$OUT/$name/normals/model.gltf" "$OUT/$name/welded/model.gltf" > /dev/null
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" \
    "$(field "$name/normals" 'd["primitives"]["added"]')" "$(field "$name/normals" 'd["primitives"]["smoothed"]')" \
    "$(field "$name/normals" 'd["primitives"]["kept"]')" \
    "$(field "$name/weld-alone" 'd["input"]["vertices"] + 0')" "$(field "$name/weld-after" 'd["input"]["vertices"]')" \
    "$seconds" "$rss" \
    "$(field "$name/weld-alone" 'd["output"]["vertices"]')" "$(field "$name/weld-after" 'd["output"]["vertices"]')"
  rm -rf "$OUT/$name/alone" "$OUT/$name/welded"
done
