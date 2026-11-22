#!/bin/bash
# WebP against AVIF on the sources of sources.sh (docs/experiments/webp-avif.md): size, PSNR and SSIM
# against the source, encode time and decode time, through vips (libwebp; libheif with aom to encode
# and dav1d to decode, as sharp and therefore glTF-Transform's textureCompress use them).
# usage: webp-avif.sh <srcdir> <outdir> <csv> [image...]. Writes only to <outdir> and <csv>.
#
# Times are wall seconds and CPU seconds (user + sys, all threads): encoders and decoders thread
# differently, and a browser decodes several images at once, so CPU time is the fairer cost.
# Encode times include decoding the PNG source; the "png" row of each image is that alone.
# Decode times are `vips avg`, which decodes every pixel and adds little else.
set -euo pipefail
SRC=$1 OUT=$2 CSV=$3; shift 3
IMAGES=("$@"); [ ${#IMAGES[@]} -gt 0 ] || IMAGES=(color2k color4k color8k normal2k normal8k orm2k)
mkdir -p "$OUT"
[ -f "$CSV" ] || echo "image,format,config,encode_s,encode_cpu_s,encode_rss_mib,bytes,psnr,ssim,decode_s,decode_cpu_s" > "$CSV"
# timed FILE COMMAND...: runs COMMAND, writes "wall cpu maxrss_kib" to FILE.
timed() {
  python3 -I -c '
import resource, subprocess, sys, time
t = time.monotonic()
r = subprocess.run(sys.argv[2:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
e = time.monotonic() - t
u = resource.getrusage(resource.RUSAGE_CHILDREN)
open(sys.argv[1], "w").write(f"{e:.3f} {u.ru_utime + u.ru_stime:.3f} {u.ru_maxrss}\n")
sys.exit(r.returncode)' "$@"
}
metric() { magick compare -metric "$1" "$2" "$3" null: 2>&1 | awk '{print $1}' || true; }
row() { # row IMAGE FORMAT CONFIG FILE VIPS ARGS...
  local img=$1 fmt=$2 cfg=$3 file=$4; shift 4
  timed "$OUT/t" vips "$@"; read -r ew ec em < "$OUT/t"
  timed "$OUT/t" vips avg "$file"; read -r dw dc _ < "$OUT/t"
  vips copy "$file" "$OUT/d.png[compression=1]"
  echo "$img,$fmt,$cfg,$ew,$ec,$((em / 1024)),$(stat -c %s "$file"),$(metric PSNR "$SRC/$img.png" "$OUT/d.png"),$(metric SSIM "$SRC/$img.png" "$OUT/d.png"),$dw,$dc" >> "$CSV"
  rm -f "$file" "$OUT/d.png"
}
for img in "${IMAGES[@]}"; do
  s="$SRC/$img.png"
  timed "$OUT/t" vips avg "$s"; read -r pw pc _ < "$OUT/t"
  echo "$img,png,source,$pw,$pc,,$(stat -c %s "$s"),,,$pw,$pc" >> "$CSV"
  for q in 50 65 80 90 95; do
    row "$img" webp "q$q" "$OUT/o.webp" copy "$s" "$OUT/o.webp[Q=$q,effort=4,keep=none]"
  done
  for e in 0 6; do row "$img" webp "q80 effort$e" "$OUT/o.webp" copy "$s" "$OUT/o.webp[Q=80,effort=$e,keep=none]"; done
  row "$img" webp "q80 smart" "$OUT/o.webp" copy "$s" "$OUT/o.webp[Q=80,effort=4,smart_subsample,keep=none]"
  for q in 30 45 60 75 90; do
    row "$img" avif "q$q" "$OUT/o.avif" copy "$s" "$OUT/o.avif[Q=$q,compression=av1,effort=4,keep=none]"
  done
  for e in 0 2 6; do row "$img" avif "q60 effort$e" "$OUT/o.avif" copy "$s" "$OUT/o.avif[Q=60,compression=av1,effort=$e,keep=none]"; done
  case $img in normal* | orm*)
    # Data maps: WebP lossy is always 4:2:0; AVIF can keep full chroma.
    for q in 45 60 75; do
      row "$img" avif "q$q 444" "$OUT/o.avif" copy "$s" "$OUT/o.avif[Q=$q,compression=av1,effort=4,subsample_mode=off,keep=none]"
    done
    row "$img" webp lossless "$OUT/o.webp" copy "$s" "$OUT/o.webp[lossless,effort=4,keep=none]"
    ;;
  esac
done
rm -f "$OUT/t"
