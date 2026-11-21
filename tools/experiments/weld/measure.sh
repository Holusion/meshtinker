#!/usr/bin/env bash
# weld's effect on simplify and compress (docs/weld.md#results). For each
# model: import into two chains, weld one, then simplify and compress both;
# and compress the import and the weld stage alone, as files. One
# tab-separated line per model on stdout. Read-only on the models; everything
# is written under OUT.
#
#   tools/experiments/weld/measure.sh BUILD OUT MODEL...
set -euo pipefail
BUILD=$1 OUT=$2; shift 2
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
stage() { echo "$1/$(ls "$1" | grep '^[0-9]' | sort | sed -n "${2}p")"; }
bins() { du -cb "$1"/*.bin | tail -1 | cut -f1; }
printf 'model\tvertices\twelded\tweld_s\tweld_rss_kb\tfaces\tsimplified\twelded_simplified\tsimplify_s\twelded_simplify_s\tbin\tcompressed\twelded_compressed\tsimplified_compressed\twelded_simplified_compressed\n'
for model in "$@"; do
  name=$(basename "$(dirname "$model")")-$(basename "${model%.*}")
  rm -rf "${OUT:?}/$name"; mkdir -p "$OUT/$name/files-plain" "$OUT/$name/files-welded"
  "$BUILD/meshtinker-import" "$model" "$OUT/$name/plain" > /dev/null
  "$BUILD/meshtinker-import" "$model" "$OUT/$name/welded" > /dev/null
  read -r weld_s weld_rss < <(run "$name/weld" weld "$OUT/$name/welded")
  imported=$(stage "$OUT/$name/plain" 1); welded=$(stage "$OUT/$name/welded" 2)
  run "$name/compress-only-plain" compress "$(ls "$imported"/*.gltf)" "$OUT/$name/files-plain/model.gltf" > /dev/null
  run "$name/compress-only-welded" compress "$(ls "$welded"/*.gltf)" "$OUT/$name/files-welded/model.gltf" > /dev/null
  read -r simplify_s _ < <(run "$name/simplify-plain" simplify "$OUT/$name/plain")
  read -r wsimplify_s _ < <(run "$name/simplify-welded" simplify "$OUT/$name/welded")
  run "$name/compress-plain" compress "$OUT/$name/plain" > /dev/null
  run "$name/compress-welded" compress "$OUT/$name/welded" > /dev/null
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" \
    "$(field "$name/weld" "d['input']['vertices']")" "$(field "$name/weld" "d['output']['vertices']")" "$weld_s" "$weld_rss" \
    "$(field "$name/simplify-plain" "d['input']['faces']")" "$(field "$name/simplify-plain" "d['output']['faces']")" \
    "$(field "$name/simplify-welded" "d['output']['faces']")" "$simplify_s" "$wsimplify_s" \
    "$(bins "$imported")" "$(bins "$OUT/$name/files-plain")" "$(bins "$OUT/$name/files-welded")" \
    "$(bins "$(stage "$OUT/$name/plain" 3)")" "$(bins "$(stage "$OUT/$name/welded" 4)")"
done
