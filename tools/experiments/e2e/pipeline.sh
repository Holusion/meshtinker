#!/bin/bash
# Runs the whole pipeline as a chain on each model and checks the result:
#   [TEXTURES=avif] tools/experiments/e2e/pipeline.sh BUILD_DIR OUT_DIR MODEL...
# import -> normals -> weld -> dedup -> simplify -> uv -> ktx (or avif) -> compress -> export as GLB, in the
# step order of docs/chaining.md. Prints each command's exit code, time and
# warnings, then validates the GLB against the input (tools/validate.mjs) and compares it with the chain's
# last stage (tools/compare.mjs), which export must write unchanged. OUT_DIR must be on a real disk.
set -u
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
export LC_NUMERIC=C
BUILD=$1; OUT=$2; shift 2
TEXTURES=${TEXTURES:-ktx}
for model in "$@"; do
  name=$(basename "$(dirname "$model")")-$(basename "${model%.*}")
  dir="$OUT/$name"; rm -rf "$dir"; mkdir -p "$dir"
  work="$dir/work"
  echo "== $name"
  status=0
  for step in "import $model $work" "normals $work" "weld $work" "dedup $work" "simplify $work" "uv $work" "$TEXTURES $work" "compress $work" "export $work $dir/out.glb"; do
    cmd=${step%% *}
    start=$(date +%s.%N)
    "$BUILD/meshtinker" ${step%% *} --json ${step#* } > "$dir/$cmd.json" 2> "$dir/$cmd.err"
    code=$?
    seconds=$(echo "$(date +%s.%N) - $start" | bc)
    warnings=$(python3 -c "import json,sys
try: r=json.load(open(sys.argv[1]))
except Exception: print('(no json)'); sys.exit()
print(' '.join(f\"{d['code']}x{d.get('count',1)}\" for d in r.get('diagnostics',[])))" "$dir/$cmd.json")
    printf '  %-9s exit %d  %6.1f s  %s\n' "$cmd" "$code" "$seconds" "$warnings"
    [ $code -ne 0 ] && { status=1; tail -2 "$dir/$cmd.err"; break; }
  done
  [ $status -ne 0 ] && continue
  last=$(ls -d "$work"/0* 2>/dev/null | tail -1)
  node "$REPO/tools/validate.mjs" "$dir/out.glb" --against "$model" > "$dir/validate.json" 2>&1
  python3 -c "import json,sys; r=json.load(open(sys.argv[1])); print('  validator: errors', r['output']['errors'], 'new issues', r['newIssues'])" "$dir/validate.json" 2>/dev/null || echo "  validator failed"
  stage_gltf=$(ls "$last"/*.gltf | head -1)
  node "$REPO/tools/compare.mjs" "$stage_gltf" "$dir/out.glb" > "$dir/compare.json" 2>&1
  python3 -c "import json,sys; r=json.load(open(sys.argv[1])); print('  export against last stage: equal', r['equal'], [d['path'] for d in r.get('differences',[])][:3])" "$dir/compare.json" 2>/dev/null || { echo "  compare failed:"; tail -2 "$dir/compare.json"; }
  echo "  size: $(du -sb "$work"/000-* | cut -f1) -> $(stat -c %s "$dir/out.glb") bytes"
done
