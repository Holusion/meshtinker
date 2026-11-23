# avif

`meshtinker avif` encodes the images of the scene as AVIF and references them through `EXT_texture_avif`. It is the web-image alternative to [`ktx`](ktx.md): smaller downloads, but textures that decode to RGBA8 on the GPU, where KTX2 stays block-compressed ([webp-avif.md](experiments/webp-avif.md#support)). Decoding, resizing, memory and threads are shared with other commands and described in [images.md](images.md). Choosing the textures to encode, and pointing them at the new files, is shared with `ktx` (`texture::reencode`, `include/meshtinker/texture/reencode.hpp`).

```bash
meshtinker avif in.gltf out/                    # quality High, effort 3
meshtinker avif in.gltf out/ --quality Low --effort 2 --max-size 2048
```

## Encoder

libavif with libaom's AV1 encoder, as sharp, and therefore glTF-Transform's `textureCompress`, uses them. AVIF is about 25 % smaller than WebP at equal rendered SSIMULACRA2, with the same browser and three.js support for 0.5 % fewer users ([webp-avif.md](experiments/webp-avif.md)). That's why it is AVIF and not WebP.

- **Versions:** libavif 1.4.2 and libaom 3.15.3, built from release archives that the build downloads, with nasm 3.02 for libaom's x86 code ([vendor/README.md](../vendor/README.md#downloaded-at-build-time)). They are compiled into `meshtinker-avif`, and nothing is loaded at run time. A build without network access sets `-DMESHTINKER_AVIF=OFF`, or `MESHTINKER_DOWNLOAD_DIR` to a folder holding the archives.
- **libaom's mode:** libavif encodes a still image in libaom's all-intra mode, with `tune=iq`, libaom's tuning for perceptual quality on still images, from libaom 3.13. That tuning, and full chroma on data maps, is why `avif`'s output scores 0.5 to 8 points of SSIMULACRA2 above the study's vips AVIF at the same quality and a similar size ([Results](#results)).
- **Tiles:** libavif's automatic tiling splits a large image so that libaom spreads it over every thread.
- **Conversion:** RGB to YUV is libavif's own, without libyuv.

## Per-slot settings

| Image | Pixel format | Why |
| --- | --- | --- |
| Color only (base color, emissive, sheen and specular color, spec-gloss) | 4:2:0 | Chroma at half resolution, as photos are stored: the eye sees less of it. |
| Normal, metallic-roughness, occlusion and other data, or used as both color and data | 4:4:4 | Each channel holds its own value; halving two of them would blur normals and roughness. |
| Gray source (1 or 2 channels) | 4:0:0 | Luma only: an occlusion or roughness map stored in gray. |

- **Alpha** is kept only when a slot samples it and the image has an alpha below 255 (`image::analyze`, as in `ktx`), at the same quality as color.
- **Color description:** every image gets BT.709 primaries, the sRGB transfer function, the BT.601 matrix and full range (CICP 1/13/6). Browsers then convert nothing, as for an untagged PNG or JPEG. three.js reads a slot as sRGB or linear by itself, whatever the file says.
- **Mixed use:** an image used both as color and as data is encoded, in 4:4:4, rather than left as it is as `ktx` does: AVIF's transfer function is a tag, and one file serves both.
- **8 bits per channel**, as the decoded source.

## Quality and effort

`--quality` takes eCorpus's preset names, as `ktx` does, and maps them to libavif's quality (0 to 100, the scale of sharp's and vips's `quality`):

| Preset | Highest | High (default) | Medium | Low | Thumb |
| --- | --- | --- | --- | --- | --- |
| libavif quality | 80 | 65 | 50 | 40 | 30 |

The steps come from the rendered comparison's median SSIMULACRA2 on 3× close-ups, at effort 4: quality 80 gave 87, 65 gave 82, 50 gave 74, 35 gave 60 ([webp-avif.md](experiments/webp-avif.md#rendered-models)).

`--effort` is sharp's and vips's scale, 0 (fastest) to 9, so that glTF-Transform and eCorpus settings carry over; libavif's speed is 9 − effort. The default is 3, the knee of the [effort sweep](experiments/webp-avif.md#effort-where-results-become-mostly-good):
- effort 3 writes 10 % more bytes than effort 6, at the same rendered score;
- effort 4 saves 6 % of that, for 4 times effort 3's time;
- efforts 0 and 1 made some textures 2 to 140 times larger in the sweep, which is why the help warns against them.

## When the source stays

An image whose AVIF isn't smaller than its source keeps its source, and its texture keeps its core `source`. AVIF's container costs about 300 bytes, so a 16×16 PNG placeholder, as some scans carry, is smaller as it is. Such images are listed under `notSmaller` in the report.

## Textures and sources

- An encoded texture gets `extensions.EXT_texture_avif.source`, pointing to the new image (`mimeType: image/avif`).
- **The core `source` is removed,** and `EXT_texture_avif` goes into `extensionsUsed` and `extensionsRequired`, as `ktx` does with `KHR_texture_basisu`. A browser without AVIF (Safari and iOS before 16, Edge before 121) can't load the model. A PNG or JPEG fallback would make the output larger than its input.
- **An `EXT_texture_webp` alternative goes too,** with the source it was an alternative to, and the extension leaves the lists once no texture uses it.
- **KTX2 textures stay as they are:** they are block-compressed on the GPU, which AVIF isn't, and `avif` can't decode them. So do textures that are already AVIF.
- **Left as they are, each with a counted warning:**
  - a texture whose only source is WebP in a build without libwebp (`image-format-unsupported`), as is an image in a format other than PNG, JPEG and WebP; where WebP is decoded, a WebP-only texture is encoded, and kept when its AVIF isn't smaller;
  - with `--min-size N`, an image whose longest side is N or less, without a warning;
  - an image whose file is missing (`image-missing`), that can't be decoded (`image-decode-failed`), or that libavif fails on (`image-encode-failed`);
  - an image only slots unknown to `avif` use (`image-slot-unknown`).
- **Files:** the new image keeps the source image's `name`, `extras` and extensions, and its file keeps the source's stem: `wall.jpg` becomes `wall.avif`. Each file is written as soon as it is encoded.

`ktx` and `avif` leave each other's textures alone: `ktx` can't decode AVIF, and `avif` keeps KTX2. Run one of them, after every other image command ([chaining.md](chaining.md#step-order)).

## Report

- **Per image:** the source's name, format and size, the output's index, name and size, its pixel format, whether alpha was kept, bytes before and after, and the time.
- **Totals:** images encoded, left as they are and kept because they were smaller, bytes before and after, and the time; then the files written.

Text by default, JSON with `--json`.

In a chain, each encoded image adds an `imageEncoding` round to `lossy` ([chaining.md](chaining.md)): `{image, format: "image/avif", quality, effort, subsampling}`. An earlier round, such as the one `import` records for a JPEG source, makes it warn `repeated-lossy-image-encoding`.

## Results

Release build, 4 cores, on collection models (`tools/monitor.py`; heap is peak `RssAnon`):

| Model | Images | Image bytes | Time | Heap |
| --- | --- | --- | --- | --- |
| `01-akhenaton-nefertiti` | one 4k color | 4.5 MB → 1.33 MB | 3.4 s | 297 MiB |
| `28-tasse-100k` | 4k color, 4k normal | 3.6 MB → 0.70 MB | 3.6 s | 491 MiB |
| `27-athlete-clean-01` | three 4k (color, normal, ORM) | 12.8 MB → 3.96 MB | 8.2 s | 482 MiB |
| `16-hermaphrodite-export-02` | one 8k color | 8.3 MB → 1.94 MB | 13.6 s | 1.22 GiB |
| `23-man-10918-pyxide` | three 8k (color, normal, occlusion) | 34.0 MB → 8.51 MB | 25.1 s | 1.62 GiB |
| `15-ulysseilescalypso` | 2k and 4k, plus a 16×16 kept as PNG | 6.5 MB → 1.91 MB | 3.6 s | 271 MiB |

- **Memory:** an 8k image peaks at about 1.2 GiB in 4:2:0 and 1.6 GiB in 4:4:4: the decoded RGBA (256 MiB), its YUV planes, and libaom's working set with its tiles. That's under half of an 8k ETC1S encode's 3.5 GiB ([fast-textures.md](experiments/fast-textures.md)). Images are encoded one at a time, as [images.md](images.md#memory-and-threads) says.
- **Rendered quality:** rendered with the [comparison's](experiments/webp-avif.md#rendered-models) harness, these outputs score 0.5 to 8 points of SSIMULACRA2 above the study's vips AVIF at the same quality (65, effort 4), in about the same bytes. Two models with data maps are 7 and 16 % larger, which is full chroma's cost, and score 1 to 7 points higher.

| Model | `avif` (quality 65, effort 3) | vips AVIF (quality 65, effort 4) |
| --- | --- | --- |
| `01-akhenaton-nefertiti` | 1.33 MB; 92.6 whole, 85.2 near, 76.5 close | 1.43 MB; 91.9, 83.8, 75.9 |
| `23-man-10918-pyxide` | 8.51 MB; 93.4, 84.1, 71.0 | 7.31 MB; 92.3, 81.4, 67.8 |
| `27-athlete-clean-01` | 3.96 MB; 94.0, 85.3, 72.6 | 3.70 MB; 92.9, 80.8, 65.2 |
| `15-ulysseilescalypso` | 1.91 MB; 92.9, 82.0, 68.9 | 1.91 MB; 90.2, 75.2, 60.6 |

## Validation

There's no reference output to compare against ([validation.md](validation.md)). ctest (`test/avif/avif_test.cpp`) decodes every output with libavif and checks, on the `avif` fixture (noisy 48×48 images, which AVIF stores smaller than PNG):

- each file's pixel format, alpha plane and color description, against the table above, and that `readImageHeader` reads its size;
- PSNR against the decoded source between 20 and 60 dB: the fixture's added noise is what AVIF smooths away;
- the 4×4 PNG kept as it is, with its core source;
- the WebP alternative removed, and the KTX2 and AVIF textures kept; the 8×8 WebP-only texture kept as smaller than its AVIF, or with a warning where the build doesn't decode WebP;
- lower presets writing smaller files further from the source;
- `--max-size`, without rounding sizes to a block multiple;
- in a chain (`test/commands/chain_test.cpp`), one round per image, nothing to encode on a second run, and `ktx` leaving the AVIF textures with its warning.

On real models, `validate.mjs --against` reports only the [known](validation.md#known-validator-differences) `EXT_texture_avif` codes, and `compare.mjs` differs only in the encoded textures' `image` and `mimeType`. No viewer has opened an `avif` output yet.
