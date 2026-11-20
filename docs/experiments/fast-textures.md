# Experiment: textures for a fast preset

This experiment decides how a fast pipeline should write textures: KTX2 at reduced size, a downscaled WebP, AVIF or JPEG, or two of them, kept side by side because each wins on different models. Its result decides whether `ktx` gets a fast mode ([ktx.md](../ktx.md#no-fast-mode)) and what `optimize`'s fast preset does with textures (TODO.md, meta-operations).

## What "fast" means

A fast preset produces a temporary model, about 80 % as good, while the final asset is being made. It is only worth having if it is about an order of magnitude faster than the default (the default being `ktx` with ETC1S at full size). A speedup of 1.5 or 2 is a slower default, not a fast preset.

## Questions

1. **Speed.** For each format, the encode time at full, half and quarter size, against the `ktx` default on the same image. Decoding the source and resizing count too: the whole step from the source image to the written file.
2. **File size**, which the viewer downloads.
3. **Quality:** PSNR against the full-size source. SSIM too, if PSNR ranks the candidates differently from a look at the images.
4. **Viewer memory.** KTX2 stays block-compressed on the GPU: ETC1S transcodes to BC1/ETC1 (4 bits per texel), UASTC to BC7/ASTC (8 bits). WebP, AVIF and JPEG are decoded to RGBA8 (32 bits per texel), plus a mip chain. On large models (many 8k pages), this decides whether the temporary model can load at all.
5. **The encoder's own RAM.** An 8k ETC1S encode peaks at 3.5 GiB. Peak RSS per format and size, for the "one image at a time" budget ([images.md](../images.md#memory-and-threads)).
6. **One format or two.** Is one format clearly better for fast approximate results on every model, or does it depend on the model's size? For example, a JPEG for small models, and UASTC at half size for large ones, where viewer memory decides. If two are needed, give the rule that picks between them from the model's figures (texel count, `inspect`'s report).

## Measurements so far: KTX2

basisu v2.50, 4 threads, on two scan color maps (sources below). PSNR is against the full-size source, after decoding to RGBA8 (`ktx extract --transcode rgba8`) and scaling back up to the source size, so it includes the loss from resizing. Another session was loading the machine during these runs: compare times as ratios within one image, not across images, and rerun on an idle machine before deciding.

| Image | Setting | Time | Speedup | Size | PSNR | Peak RSS |
| --- | --- | --- | --- | --- | --- | --- |
| 4k | ETC1S `-q 240 -comp_level 1` (default) | 16.9 s | 1 | 2.56 MB | 32.8 dB | 882 MiB |
| 4k | ETC1S `-comp_level 0` | 8.8 s | 1.9 | 2.44 MB | 32.3 dB | 843 MiB |
| 4k | ETC1S `-comp_level 0`, no RDO, `-q 128` | 7.5 s | 2.3 | 2.23 MB | 31.4 dB | 835 MiB |
| 4k | UASTC level 0, Zstd 1 | 3.3 s | 5 | 17.3 MB | 45.8 dB | 331 MiB |
| 4k | ETC1S `-comp_level 0`, half size | 2.7 s | 6 | 0.72 MB | 28.9 dB | 242 MiB |
| 4k | UASTC level 0, Zstd 1, half size | 1.05 s | 16 | 4.93 MB | 30.9 dB | 103 MiB |
| 4k | ETC1S `-comp_level 0`, quarter size | 0.93 s | 18 | 0.24 MB | 26.9 dB | 93 MiB |
| 8k | ETC1S `-q 240 -comp_level 1` (default) | 132 s | 1 | 11.7 MB | 36.3 dB | 3.5 GiB |
| 8k | ETC1S `-comp_level 0` | 76 s | 1.7 | 11.1 MB | 35.8 dB | 3.4 GiB |
| 8k | UASTC level 0, Zstd 1 | 11.2 s | 12 | 79.6 MB | 52.8 dB | 1.3 GiB |
| 8k | ETC1S `-comp_level 0`, half size | 10.3 s | 13 | 3.0 MB | 33.7 dB | 1.0 GiB |
| 8k | UASTC level 0, Zstd 1, half size | 3.0 s | 43 | 20.6 MB | 36.6 dB | 347 MiB |
| 8k | ETC1S `-comp_level 0`, quarter size | 2.9 s | 46 | 0.82 MB | 31.5 dB | 307 MiB |

- No ETC1S setting at full size gains more than about 2. ETC1S's codebook search is the cost, and only fewer texels cut it.
- **UASTC level 0 at half size** is the best KTX2 candidate:
  - 16 to 43 times faster than the default;
  - PSNR 1.9 dB below the default on the 4k image, 0.3 dB above it on the 8k one;
  - files about 1.8 times larger;
  - half the GPU memory of the full-size ETC1S image: 8 bits per texel over a quarter of the texels, against 4 bits.
- ETC1S at half size is 6 to 13 times faster with files 4 times smaller, at 2.7 to 3.9 dB below the default.
- The resize (ImageMagick, not timed above) has to be counted in the comparison: it is part of every downscaled candidate.

## Still to measure

- **WebP, AVIF and JPEG** at full, half and quarter size, with fast encoder settings (WebP `method` 0 to 2, AVIF speed 8 to 10, JPEG quality 80 to 90), on the same images, with the same metrics as the KTX2 rows.
- **More images:** at least a normal map and an ORM map (sources below), and a model made of many small pages (`32-campus-romainville`, 634 primitives of 256² to 512² images), where per-image overhead may dominate.
- **Viewer memory per model:** the sum over the model's images of the GPU size, for each candidate. The figure can be computed rather than measured: texels × bits per texel × 4/3 for mips.

On the machine: ImageMagick 7.1.1 reads and writes WebP (libwebp 1.5.0) and AVIF (libheif 1.23.6), and `vips` is installed. There's no standalone `cwebp`, `avifenc` or `cjpeg`. Time the encoders through `vips` (`vips webpsave`, `heifsave --compression av1`, `jpegsave`), which has less overhead than ImageMagick's 16-bit pipeline, or ask before installing the command-line encoders. Whatever wins must then be built into meshtinker as a vendored library (AGENTS.md). For JPEG, stb_image_write is already vendored but is slow and makes large files, and libjpeg-turbo would be the alternative. For WebP that means libwebp. For AVIF, libavif with an AV1 encoder, which is large. Note the build cost of each candidate in the result.

## Setup

The scripts are in `tools/experiments/textures/`. They write only to the directories given as arguments: use the session's scratchpad, never the repository or the models' folders.

1. **Sources:** `sources.sh <dir>` writes the PNG sources (compression level 1, so that every encoder decodes the same input). It reads the local models in place, read-only, and DamagedHelmet from `test/assets` (`tools/fetch-assets.sh`):
   - `color2k`: `collection/output/40-chauvet-sdf403/baseColor_1.jpg`, 2048²;
   - `color4k`: `collection/output/01-akhenaton-nefertiti/Akhenaton_Nefertiti.jpg`, 4000²;
   - `color8k`: `collection/output/17-golem/golem_u0_v0_diffuse.jpg`, 8192²;
   - `normal8k`: Notre Dame en Vaux `..._10M_normal.1003.jpg`, 8192²;
   - `normal2k` and `orm2k`: DamagedHelmet's normal and metallicRoughness maps, 2048² (`glb_images.py` extracts them).
2. **basisu:** build v2.50 from source: `git clone --depth 1 --branch v2_50 https://github.com/BinomialLLC/basis_universal`, then `cmake -DCMAKE_BUILD_TYPE=Release -DBASISU_EXAMPLES=OFF` and build. The binary is `bin/basisu` in the source tree. `ktx` 4.4.2 (`/usr/bin/ktx`) is eCorpus's baseline, and decodes KTX2 for the PSNR.
3. **Runs:**
   - `fast.sh <basisu> <srcdir> <outdir> <csv>` measures the KTX2 rows above.
   - `bench.sh` measures the default-preset grid for [ktx.md](../ktx.md#presets): ETC1S quality and compression levels, and UASTC levels and Zstd on data maps, against `ktx` 4.4.2 with eCorpus's arguments.
   - `timeit.py` records the wall time and peak RSS of one command, since GNU `time` isn't installed.
   - Add a script for the WebP, AVIF and JPEG rows in the same CSV shape (image, config, seconds, peak RSS, bytes, PSNR against the full-size source).
4. Run on an idle machine (4 cores, 31 GB). Check `uptime` before and after, and pause or rerun rows timed under load.

## Deliverables

- The rows for every candidate, added to the table above.
- An answer to each question.
- A recommendation: the fast texture path (format, size, encoder settings), or two paths and the rule between them. It may also be "no fast textures".
- The library each recommended path needs, with its build cost.
