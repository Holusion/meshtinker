#!/bin/sh
# For each image: shrink by 2 and 4 (lanczos3), enlarge back, and print PSNR and SSIM against the original.
# Whole images, padding included: see docs/experiments/many-textures.md for what that biases.
#   downup.sh IMAGE...
for src in "$@"; do
  w=$(vipsheader -f width "$src"); h=$(vipsheader -f height "$src")
  line="$(basename $src) ${w}x${h}"
  for f in 2 4; do
    t=$(mktemp -d)
    vips resize "$src" $t/d.png $(echo "1/$f" | bc -l) --kernel lanczos3
    vips resize $t/d.png $t/u.png $f --kernel lanczos3
    vips crop $t/u.png $t/uc.png 0 0 $w $h 2>/dev/null || cp $t/u.png $t/uc.png
    p=$(magick compare -metric PSNR "$src" $t/uc.png null: 2>&1 | cut -d' ' -f1)
    s=$(magick compare -metric SSIM "$src" $t/uc.png null: 2>&1 | cut -d' ' -f1)
    line="$line | /$f psnr $p ssim $s"
    rm -rf $t
  done
  echo "$line"
done
