#!/bin/bash
# Runs inspect, then optimize (slow preset, High, KTX2, or OPTIMIZE_OPTIONS), on each model, for docs/experiments/optimize-survey.md:
#   tools/experiments/optimize/survey.sh BUILD_DIR OUT_DIR TIMEOUT_SECONDS MODEL...
# Writes OUT_DIR/<name>.inspect.json, <name>.optimize.json (optimize --json) and <name>.run (exit code and
# wall time). A run that reaches the timeout gets SIGTERM, which optimize forwards to the running step. The
# output GLB is deleted once its size is in the JSON: OUT_DIR must be on a real disk with room for one model's
# chain.
set -u
BUILD=$1; OUT=$2; TIMEOUT=$3; shift 3
mkdir -p "$OUT"
for model in "$@"; do
  name=$(basename "$(dirname "$model")")-$(basename "${model%.*}")
  [ -e "$OUT/$name.run" ] && continue
  "$BUILD/meshtinker-inspect" --json "$model" > "$OUT/$name.inspect.json" 2> /dev/null
  start=$(date +%s.%N)
  timeout --signal=TERM "$TIMEOUT" "$BUILD/meshtinker-optimize" --json ${OPTIMIZE_OPTIONS:-} "$model" "$OUT/$name.glb" \
    > "$OUT/$name.optimize.json" 2> "$OUT/$name.stderr"
  code=$?
  echo "$code $(echo "$(date +%s.%N) - $start" | bc)" > "$OUT/$name.run"
  echo "$name exit $code $(cut -d' ' -f2 "$OUT/$name.run") s"
  rm -f "$OUT/$name.glb"
done
