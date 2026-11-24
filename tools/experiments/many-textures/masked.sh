#!/bin/sh
# The pilot's measures, masked to the texels the scene samples (docs/experiments/many-textures.md):
#   masked.sh MASK IMAGE [LABEL]
# MASK is `inspect --occupancy-maps`'s map of IMAGE (white where sampled), at IMAGE's size. IMAGE is PNG or JPEG;
# decode KTX2 first (ktx extract --transcode rgba8). Prints one CSV line:
#   label,occupancy,psnr2,psnr4,e0,e1,e2,e0/e1,e1/e2,k0,b8,b4
# - psnr2, psnr4: shrunk by 2 and 4 (lanczos3), enlarged back, PSNR over RGB on the masked texels only.
# - e0..e2: mean square of the luma's Laplacian pyramid bands 0 to 2, weighted by the mask (box-shrunk with each
#   level); k0: band 0's kurtosis, on the masked texels.
# - b8, b4: band 0's energy on texels next to an 8×8 (JPEG) or 4×4 (ETC1S) block boundary, over its energy on the
#   others, masked. Near 1 without blocking; over 1 when block edges add energy of their own.
# Scratch files go to $TMPDIR (default build/exp-texels/tmp), on disk: a 4k image takes a few hundred MB.
export LC_ALL=C
mask=$1; src=$2; label=${3:-$(basename "$src")}
tmp=${TMPDIR:-build/exp-texels/tmp}; mkdir -p "$tmp"; t=$(mktemp -d -p "$tmp")
w=$(vipsheader -f width "$src"); h=$(vipsheader -f height "$src")
avg() { vips avg "$1"; }
# Weighted mean of $1 by weights $2 (a 0..1 float image): avg(a·w) / avg(w).
wmean() { vips multiply "$1" "$2" $t/wm.v; echo "$(avg $t/wm.v) $(avg $2)" | awk '{ print ($2 > 0 ? $1 / $2 : 0) }'; }

vips extract_band "$mask" $t/m8.v 0
vips linear $t/m8.v $t/m0.v 0.00392156862745098 0
occupancy=$(avg $t/m0.v)

# Down-up PSNR, RGB, masked.
vips extract_band "$src" $t/rgb.v 0 --n 3 2>/dev/null || vips copy "$src" $t/rgb.v
vips cast $t/rgb.v $t/rgbf.v float
psnr() {
  vips resize $t/rgbf.v $t/d.v $(echo "1/$1" | bc -l) --kernel lanczos3
  vips resize $t/d.v $t/u.v $1 --kernel lanczos3
  vips crop $t/u.v $t/uc.v 0 0 $w $h 2>/dev/null || cp $t/u.v $t/uc.v
  vips subtract $t/rgbf.v $t/uc.v $t/e.v; vips multiply $t/e.v $t/e.v $t/e2.v
  mse=$(wmean $t/e2.v $t/m0.v)
  echo "$mse" | awk '{ printf "%.2f", ($1 > 0 ? 10 * log(65025 / $1) / log(10) : 99) }'
}
p2=$(psnr 2); p4=$(psnr 4)

# Octave bands of the luma.
vips colourspace "$src" $t/l.v b-w; vips extract_band $t/l.v $t/g.v 0; vips cast $t/g.v $t/f0.v float
for k in 0 1 2; do vips resize $t/f$k.v $t/f$((k + 1)).v 0.5 --kernel lanczos3; done
cp $t/m0.v $t/w0.v
for k in 0 1 2; do vips shrink $t/w$k.v $t/w$((k + 1)).v 2 2; done
band() { # $1 level: band image b$1.v, and its weighted mean square
  vips resize $t/f$(($1 + 1)).v $t/u.v 2 --kernel lanczos3
  vips crop $t/u.v $t/uc.v 0 0 $(vipsheader -f width $t/f$1.v) $(vipsheader -f height $t/f$1.v) 2>/dev/null || cp $t/u.v $t/uc.v
  vips subtract $t/f$1.v $t/uc.v $t/b$1.v; vips multiply $t/b$1.v $t/b$1.v $t/s$1.v
  vips crop $t/w$1.v $t/wc.v 0 0 $(vipsheader -f width $t/f$1.v) $(vipsheader -f height $t/f$1.v) 2>/dev/null || cp $t/w$1.v $t/wc.v
  wmean $t/s$1.v $t/wc.v
}
e0=$(band 0); e1=$(band 1); e2=$(band 2)
vips multiply $t/s0.v $t/s0.v $t/q0.v
q0=$(wmean $t/q0.v $t/m0.v)

# Band 0 next to block boundaries (the two texels either side of each edge), against the rest.
vips xyz $t/xy.v $w $h
vips extract_band $t/xy.v $t/x.v 0; vips extract_band $t/xy.v $t/y.v 1
blocks() { # $1 block size: ratio of band-0 energy on boundary texels to the others
  last=$(($1 - 1))
  for a in x y; do
    vips remainder_const $t/$a.v $t/${a}r.v $1
    vips relational_const $t/${a}r.v $t/${a}0.v equal 0; vips relational_const $t/${a}r.v $t/${a}1.v equal $last
    vips boolean $t/${a}0.v $t/${a}1.v $t/${a}e.v or
  done
  vips boolean $t/xe.v $t/ye.v $t/edge.v or
  vips linear $t/edge.v $t/ef.v 0.00392156862745098 0; vips multiply $t/ef.v $t/m0.v $t/we.v
  vips invert $t/edge.v $t/ie.v; vips linear $t/ie.v $t/if.v 0.00392156862745098 0; vips multiply $t/if.v $t/m0.v $t/wi.v
  echo "$(wmean $t/s0.v $t/we.v) $(wmean $t/s0.v $t/wi.v)" | awk '{ printf "%.3f", ($2 > 0 ? $1 / $2 : 0) }'
}
b8=$(blocks 8); b4=$(blocks 4)

echo "$label $occupancy $p2 $p4 $e0 $e1 $e2 $q0 $b8 $b4" | awk '{
  printf "%s,%.3f,%s,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%s,%s\n", $1, $2, $3, $4, $5, $6, $7, $5 / $6, $6 / $7, $8 / ($5 * $5), $9, $10 }'
rm -rf "$t"
