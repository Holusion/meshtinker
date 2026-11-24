#!/bin/sh
# Energy of each of the finest octaves of an image's luma, as the mean square of a Laplacian pyramid band,
# and the ratio of each to the one below. A scale-invariant (1/f²) image gives ratios near 1; a soft or
# upsampled one, well under 1; noise, over 1. The kurtosis of a band tells detail (sparse, heavy-tailed: well
# over 3) from Gaussian noise (3). Whole images: smooth padding lowers every band alike, and raises kurtosis.
#   octaves.sh IMAGE...
export LC_ALL=C  # vips prints numbers in the locale, and awk reads a decimal point
band() { # $1 image at one level, $2 its half-size version: mean square of $1 - up($2)
  vips resize $2 $t/u.v 2 --kernel lanczos3
  vips crop $t/u.v $t/uc.v 0 0 $(vipsheader -f width $1) $(vipsheader -f height $1) 2>/dev/null || cp $t/u.v $t/uc.v
  vips subtract $1 $t/uc.v $t/d.v; vips multiply $t/d.v $t/d.v $t/sq.v; vips multiply $t/sq.v $t/sq.v $t/q.v
  echo "$(vips avg $t/sq.v) $(vips avg $t/q.v)"
}
for src in "$@"; do
  t=$(mktemp -d)
  vips colourspace "$src" $t/l0.v b-w; vips extract_band $t/l0.v $t/g0.v 0; vips cast $t/g0.v $t/f0.v float
  vips resize $t/f0.v $t/f1.v 0.5 --kernel lanczos3; vips resize $t/f1.v $t/f2.v 0.5 --kernel lanczos3; vips resize $t/f2.v $t/f3.v 0.5 --kernel lanczos3
  e0=$(band $t/f0.v $t/f1.v); e1=$(band $t/f1.v $t/f2.v); e2=$(band $t/f2.v $t/f3.v)
  echo "$(basename $src) $e0 $e1 $e2" | awk '{printf "%-18s E0 %7.2f E1 %7.2f E2 %7.2f  E0/E1 %.2f  E1/E2 %.2f  kurtosis K0 %.1f K1 %.1f\n", $1, $2, $4, $6, $2/$4, $4/$6, $3/($2*$2), $5/($4*$4)}'
  rm -rf $t
done
