#!/bin/bash
# Fast-mode candidates: basisu settings at full, half and quarter size.
# usage: fast.sh <basisu> <srcdir> <outdir> <csv>   (IMAGES="color4k color8k" by default)
# PSNR is against the full-size source: the decoded result is scaled back up first.
B=$1; SRC=$2; OUT=$3; CSV=$4; T="$(dirname "$0")/timeit.py"; mkdir -p "$OUT"
[ -f "$CSV" ] || echo "image,config,seconds,maxrss_mib,bytes,psnr_vs_full" > "$CSV"
run() { local img=$1 cfg=$2 in=$3; shift 3; local o="$OUT/$img-$cfg.ktx2"; rm -f "$o"
  python3 -I "$T" "$OUT/t.txt" "$B" -ktx2 -mipmap -max_threads 4 -no_stats "$@" -file "$in" -output_file "$o"
  read -r t m < "$OUT/t.txt"
  ktx extract --transcode rgba8 "$o" "$OUT/d.png" >/dev/null 2>&1
  magick "$OUT/d.png" -resize "$(identify -format '%wx%h!' "$SRC/$img.png")" "$OUT/d.png"
  echo "$img,$cfg,$t,$((m/1024)),$(stat -c %s "$o"),$(compare -metric PSNR "$SRC/$img.png" "$OUT/d.png" null: 2>&1 | awk '{print $1}')" >> "$CSV"
  tail -1 "$CSV"; }
for i in ${IMAGES:-color4k color8k}; do
  for f in 50 25; do magick "$SRC/$i.png" -resize $f% -define png:compression-level=1 "$OUT/$i-$f.png"; done
  run $i etc1s-q240-c1 "$SRC/$i.png" -q 240 -comp_level 1
  run $i etc1s-q240-c0 "$SRC/$i.png" -q 240 -comp_level 0
  run $i etc1s-q240-c0-nordo "$SRC/$i.png" -q 240 -comp_level 0 -no_endpoint_rdo -no_selector_rdo
  run $i etc1s-q128-c0-nordo "$SRC/$i.png" -q 128 -comp_level 0 -no_endpoint_rdo -no_selector_rdo
  run $i uastc-l0-z1 "$SRC/$i.png" -uastc -uastc_level 0 -ktx2_zstandard_level 1
  run $i uastc-l0-nozstd "$SRC/$i.png" -uastc -uastc_level 0 -ktx2_no_zstandard
  run $i etc1s-c0-half "$OUT/$i-50.png" -q 240 -comp_level 0
  run $i etc1s-c0-quarter "$OUT/$i-25.png" -q 240 -comp_level 0
  run $i uastc-l0-z1-half "$OUT/$i-50.png" -uastc -uastc_level 0 -ktx2_zstandard_level 1
done
