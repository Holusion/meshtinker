#!/bin/sh
# Step 2 of the two-tier budget experiment: known cases for the oversampling measures.
#   calibrate.sh OUT LABEL IMAGE MASK [LABEL IMAGE MASK...]
# For each source image (with its `inspect --occupancy-maps` mask), writes to OUT/LABEL-CASE.png:
#   sharp    the source as it is
#   up2      shrunk by 2 and enlarged back (lanczos3): one octave too large
#   up4      shrunk by 4 and enlarged back: two octaves too large
#   up2n     up2 plus Gaussian noise of σ 4 levels (sensor noise on an upsampled scan)
#   jpeg20   the source as a JPEG at quality 20: blocking on real detail
#   up2j30   up2 as a JPEG at quality 30: blocking on an upsampled image, which must still read one octave large
# then prints masked.sh's CSV line for each (label,occupancy,psnr2,psnr4,e0,e1,e2,e0/e1,e1/e2,k0,b8,b4).
set -e
export LC_ALL=C TMPDIR=${TMPDIR:-build/exp-tiers/tmp}
out=$1; shift; mkdir -p "$out" "$TMPDIR"
here=$(dirname "$0")/../many-textures
while [ $# -ge 3 ]; do
  label=$1 src=$2 mask=$3; shift 3
  w=$(vipsheader -f width "$src"); h=$(vipsheader -f height "$src")
  o=$out/$label
  vips extract_band "$src" $o-rgb.v 0 --n 3
  vips copy $o-rgb.v $o-sharp.png
  for f in 2 4; do
    vips resize $o-rgb.v $o-d.v $(echo "1/$f" | bc -l) --kernel lanczos3
    vips resize $o-d.v $o-u.v $f --kernel lanczos3
    vips crop $o-u.v $o-up$f.png 0 0 $w $h
  done
  vips gaussnoise $o-n.v $w $h --sigma 4 --mean 0
  vips bandjoin "$o-n.v $o-n.v $o-n.v" $o-n3.v
  vips add $o-up2.png $o-n3.v $o-s.v; vips cast $o-s.v $o-up2n.png uchar
  vips jpegsave $o-rgb.v $o-j.jpg --Q 20; vips copy $o-j.jpg $o-jpeg20.png
  vips jpegsave $o-up2.png $o-j.jpg --Q 30; vips copy $o-j.jpg $o-up2j30.png
  rm -f $o-*.v $o-j.jpg
  for case in sharp up2 up4 up2n jpeg20 up2j30; do
    sh "$here/masked.sh" "$mask" $o-$case.png $label-$case
  done
done
