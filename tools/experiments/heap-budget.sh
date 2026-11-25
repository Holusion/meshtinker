#!/bin/bash
# Heap budget benchmark (docs/experiments/heap-budget-results.md).
#
#   tools/experiments/heap-budget.sh RESULTS.jsonl MODELS.txt "VARIANTS" "COMMANDS" "DISKS" REPEATS
#
# MODELS.txt: one `name<TAB>path` per line. VARIANTS: build directories under build/ (budget-4m ...), each
# holding meshtinker-simplify and meshtinker-compress. COMMANDS: "simplify compress". DISKS: "ssd hdd".
# Each run: sync, record the output disk's sectors written, run under tools/monitor.py, record the
# sectors written during the run and after a final sync, then delete the output. Outputs and the
# workspace go on the chosen disk. Inputs are read once beforehand, so that they come from the page cache.
# Each disk is an output folder and the block device it is on, whose writes are counted: SSD_OUT (default
# build/heap-budget/out) on SSD_DEVICE, and HDD_OUT on HDD_DEVICE, which "hdd" needs. Devices are names in
# /sys/block, such as sda.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
RESULTS=$1; MODELS=$2; VARIANTS=$3; COMMANDS=$4; DISKS_WANTED=$5; REPEATS=$6
declare -A DISKS=([ssd]="${SSD_OUT:-$REPO/build/heap-budget/out}" [hdd]="${HDD_OUT:-}")
declare -A DEVICES=([ssd]="${SSD_DEVICE:-}" [hdd]="${HDD_DEVICE:-}")
for disk in $DISKS_WANTED; do
  if [ -z "${DISKS[$disk]:-}" ] || [ ! -e "/sys/block/${DEVICES[$disk]:-none}/stat" ]; then
    echo "heap-budget.sh: set ${disk^^}_OUT and ${disk^^}_DEVICE for the $disk runs" >&2; exit 2
  fi
done
RUNS="$REPO/build/heap-budget/runs"
mkdir -p "$RUNS" "$(dirname "$RESULTS")"
sectors() { awk '{print $7}' "/sys/block/$1/stat"; }
while IFS=$'\t' read -r name path; do
  python3 -c "
import json,sys,os
f=sys.argv[1]
if f.endswith('.glb'): print(f); sys.exit()
j=json.load(open(f)); d=os.path.dirname(f); print(f)
for u in [b['uri'] for b in j.get('buffers',[]) if 'uri' in b]+[i['uri'] for i in j.get('images',[]) if 'uri' in i]:
    if not u.startswith('data:'): print(os.path.join(d,u))" "$path" | while read -r f; do cat "$f" > /dev/null 2>&1; done
done < "$MODELS"
for rep in $(seq 1 "$REPEATS"); do
while IFS=$'\t' read -r name path; do
for cmd in $COMMANDS; do
for disk in $DISKS_WANTED; do
for variant in $VARIANTS; do
  out="${DISKS[$disk]:?}/$name-$cmd-$variant-$rep"
  run="$RUNS/$name-$cmd-$disk-$variant-$rep"
  if [ -e "$run.json" ]; then echo "skip existing $run" >&2; continue; fi
  mkdir -p "$out"
  dev=${DEVICES[$disk]}
  sync; before=$(sectors "$dev")
  python3 "$REPO/tools/monitor.py" "$run" -- "$REPO/build/$variant/meshtinker-$cmd" --json "$path" "$out/m.gltf" > /dev/null
  during=$(sectors "$dev")
  sync; after=$(sectors "$dev")
  python3 - "$run.json" "$name" "$cmd" "$disk" "$variant" "$rep" "$before" "$during" "$after" >> "$RESULTS" <<'PY'
import json,sys
m=json.load(open(sys.argv[1])); b,d,a=map(int,sys.argv[7:10])
print(json.dumps({"model":sys.argv[2],"command":sys.argv[3],"disk":sys.argv[4],"variant":sys.argv[5],"rep":int(sys.argv[6]),
  "exit":m["exit_code"],"seconds":m["elapsed_seconds"],"peak_anon_mib":round(m["peak_anon_kib"]/1024,1),
  "peak_rss_mib":round(m["peak_rss_kib"]/1024,1),"written_during_mib":round((d-b)*512/2**20),"written_total_mib":round((a-b)*512/2**20)}))
PY
  rm -rf "${out:?}"
done; done; done; done < "$MODELS"; done
