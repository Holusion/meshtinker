#!/bin/bash
# usage: bench.sh <basisu> <srcdir> <outdir> <csv>  -- runs the grid, appends rows to csv
B=$1; SRC=$2; OUT=$3; CSV=$4; mkdir -p "$OUT"
[ -f "$CSV" ] || echo "image,encoder,setting,seconds,maxrss_mib,bytes,psnr" > "$CSV"
run() { # image encoder setting cmd...
  local img=$1 enc=$2 set=$3; shift 3
  local o="$OUT/$img-$enc-$set.ktx2"; rm -f "$o" "$o.png"
  python3 -I "$(dirname "$0")/timeit.py" "$OUT/time.txt" "$@"; local t; t=$(tail -1 "$OUT/time.txt")
  [ -f "$o" ] || { echo "$img,$enc,$set,FAIL,,," >> "$CSV"; return; }
  ktx extract --transcode rgba8 "$o" "$o.png" >/dev/null 2>&1
  local p; p=$(compare -metric PSNR "$SRC/$img.png" "$o.png" null: 2>&1 | awk '{print $1}')
  echo "$img,$enc,$set,${t% *},$(( ${t#* } / 1024 )),$(stat -c %s "$o"),$p" >> "$CSV"
  rm -f "$o.png"; tail -1 "$CSV"
}
K="ktx create --generate-mipmap --threads 4"
for img in ${IMAGES:-color2k color4k color8k}; do
  for q in 128 180 220 240 250; do
    run $img ktx442 q$q-c1 $K --encode basis-lz --qlevel $q --clevel 1 --assign-tf srgb --assign-primaries bt709 --format R8G8B8_SRGB "$SRC/$img.png" "$OUT/$img-ktx442-q$q-c1.ktx2"
    for c in ${CLEVELS:-0 1 2}; do
      run $img basisu q$q-c$c $B -ktx2 -mipmap -q $q -comp_level $c -max_threads 4 -no_stats -file "$SRC/$img.png" -output_file "$OUT/$img-basisu-q$q-c$c.ktx2"
    done
  done
done
for img in ${DATA:-normal2k orm2k normal8k}; do
  case $img in normal*) BN="-normal_map";; *) BN="-linear";; esac
  for l in ${LEVELS:-0 1 2 3}; do
    run $img ktx442 l$l-z18 $K --encode uastc --uastc-quality $l --zstd 18 --assign-tf linear --assign-primaries none --format R8G8B8_UNORM "$SRC/$img.png" "$OUT/$img-ktx442-l$l-z18.ktx2"
    for z in 6 18; do
      run $img basisu l$l-z$z $B -ktx2 -uastc -uastc_level $l -ktx2_zstandard_level $z -mipmap $BN -max_threads 4 -no_stats -file "$SRC/$img.png" -output_file "$OUT/$img-basisu-l$l-z$z.ktx2"
    done
  done
done
