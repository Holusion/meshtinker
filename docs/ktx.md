# ktx

`meshtinker ktx` encodes the images of the scene as KTX2 with Basis Universal and references them through `KHR_texture_basisu`. It replaces glTF-Transform's `toktx` calls in eCorpus's `optimizeGlb.ts`. Decoding, resizing, memory and threads are shared with other commands and described in [images.md](images.md). Choosing the textures to encode, and pointing them at the new files, is shared with [`avif`](avif.md) (`texture::reencode`). The benchmark that set the presets is under [Measurements](#measurements).

```bash
meshtinker ktx in.gltf out/                    # default quality
meshtinker ktx in.gltf out/ --quality Low --max-size 2048
```

## Encoder

basis_universal (BinomialLLC, Apache-2.0) is vendored and compiled in. Its encoder library writes KTX2 itself, including Zstandard supercompression for UASTC (with Zstd vendored inside it) and mipmap generation.

The two alternatives were rejected:
- **Calling the `ktx` program**, as eCorpus does through glTF-Transform. It is a runtime dependency that users must install, and its 4.x and 5.x versions differ in flags.
- **Vendoring libktx.** It wraps the same basisu encoder, and its CMake tree is much larger. Only its KTX2 container code would be on top of what we need.

`ktx validate` (KTX-Software) is used in validation, never at runtime.

basisu writes BT.709 primaries into every file's data format descriptor, but `KHR_texture_basisu` asks for unspecified primaries with the linear transfer function, and `ktx validate --gltf-basisu` rejects the files. `ktx` sets that byte after encoding linear images, as eCorpus asks `ktx create` for with `--assign-primaries none` (`src/commands/ktx/encode.cpp`).

`KHR_texture_basisu` covers only ETC1S (BasisLZ) and UASTC LDR 4×4, with Zstd allowed on UASTC only. The newer basisu formats (ASTC/XUASTC LDR, UASTC HDR, XUBC7) are not used.

## Codec per slot

The rule is the one eCorpus uses and the spec's implementation note recommends: color data uses ETC1S, other data uses UASTC.

| Slot | Codec | Transfer function | Options |
| --- | --- | --- | --- |
| `baseColorTexture`, `emissiveTexture`, `KHR_materials_sheen` `sheenColorTexture`, `KHR_materials_specular` `specularColorTexture`, `KHR_materials_diffuse_transmission` `diffuseTransmissionColorTexture`, `KHR_materials_pbrSpecularGlossiness` `diffuseTexture` and `specularGlossinessTexture` | ETC1S | sRGB | |
| `normalTexture`, `KHR_materials_clearcoat` `clearcoatNormalTexture`, `KHR_materials_anisotropy` `anisotropyTexture` | UASTC | linear | basisu `-normal_map`: linear metrics, no RDO, renormalized mips |
| every other slot of core glTF and of the Khronos material extensions (`occlusionTexture`, `metallicRoughnessTexture`, clearcoat, transmission, volume, iridescence, sheen roughness, specular strength...) | UASTC | linear | |

- **An image used by several slots** takes UASTC if any of its slots asks for it.
- **An image used as color by one slot and as data by another** can't have a single transfer function. It is left as it is, with the warning `image-color-space-conflict`.
- **An image used only by slots the command doesn't know** (an unknown extension) is left as it is, with the warning `image-slot-unknown`. Guessing would risk the wrong transfer function, and three.js's `KTX2Loader` follows the file's transfer function. Writing a conflicting image twice, once per transfer function, was rejected: it is rare, and costs a second image.
- **Channels:** an image whose alpha is unused ([images.md](images.md#channel-analysis)) is written as RGB: its values are all 255, or no slot that samples it reads alpha (base color and spec-gloss diffuse read it unless the material is `OPAQUE`; sheen roughness, specular, diffuse transmission and spec-gloss specular always do). basisu writes RGB or RGBA descriptors only, so a gray image, such as an occlusion map, is RGB with equal channels. It is linear, never sRGB, which is what the spec requires of Red and Red-Green images.
- Images are always written with a full mip chain: the spec says an image SHOULD have one when its sampler minifies with mipmaps, or has no sampler.

## Textures and sources

- An encoded texture gets `extensions.KHR_texture_basisu.source`, pointing to the new image (`mimeType: image/ktx2`).
- **The core `source` is removed**, and `KHR_texture_basisu` goes into both `extensionsUsed` and `extensionsRequired`, as the spec says for files without fallback. This is what eCorpus does, and its viewer reads KTX2. A fallback would make the output larger for readers that eCorpus doesn't need to serve.
- The texture's other extension sources (`EXT_texture_webp`, `EXT_texture_avif`) are removed too, so that an encoded texture has its KTX2 source only. A renderer that reads KTX2 picks it anyway, and the others would only cost space. This is a stated exception to "don't strip": `ktx` replaces a texture's sources, it doesn't add one.
- `gltf::prune` then removes the images nothing uses any more, and `EXT_texture_webp` and `EXT_texture_avif` leave `extensionsUsed` and `extensionsRequired` once no texture uses them.
- The new image keeps the source image's `name`, `extras` and extensions, and its file the source's stem: `wall.jpg` becomes `wall.ktx2` (`gltf::ImageNames`, [gltf-io.md](gltf-io.md#editing)). Each file is written as soon as it is encoded.
- A texture whose only source is WebP or AVIF is encoded from it, where the build decodes that format ([images.md](images.md#formats)); in a build without libwebp or libavif it is left as it is, with `image-format-unsupported`. So is an image in another format than PNG, JPEG, WebP or AVIF, with the same warning. `--min-size N` leaves images whose longest side is N or less as they are, without a warning: `optimize` passes 512. Also left as they are, with a warning: an image whose file is missing (`image-missing`), that can't be decoded (`image-decode-failed`), or that basisu fails on (`image-encode-failed`).
- A texture already in KTX2 is kept and not re-encoded.

## Presets

The aim is few options:

- `--quality <Highest|High|Medium|Low|Thumb>` (default `High`), the same names as `optimize`, so that it passes its own straight through.
- `--max-size N` ([images.md](images.md#resizing)).

eCorpus's settings are the baseline:
- ETC1S `qlevel` by quality: Thumb 128, Low 180, Medium 220, High 240, Highest 250, `clevel` 1.
- UASTC at level 2, Zstd 18, no RDO, at every quality.
- `maxSize` by quality: 512, 1024, 2048, 4096, 8192.

`--max-size` stays explicit: `optimize` maps its quality to a size, as eCorpus does, but `ktx` alone doesn't resize unless asked.

### Settings

| `--quality` | Color slots: ETC1S | Data slots: UASTC |
| --- | --- | --- |
| `Highest` | quality level 250, compression level 1 | level 1, Zstd 18 |
| `High` (default) | quality level 240, compression level 1 | level 1, Zstd 18 |
| `Medium` | quality level 220, compression level 1 | level 1, Zstd 18 |
| `Low` | quality level 180, compression level 1 | level 1, Zstd 18 |
| `Thumb` | quality level 128, compression level 1 | level 1, Zstd 18 |

The levels are basisu's: `-q`, `-comp_level`, `-uastc_level` and `-ktx2_zstandard_level` on its command line, the same fields of its encoder parameters in the library. Every image also gets a full mip chain, and normal maps basisu's `-normal_map` ([Codec per slot](#codec-per-slot)).

- **ETC1S keeps eCorpus's settings**, which basisu v2.50 reproduces: at the same quality level and compression level 1, files are within 0.7 % of `ktx` 4.4.2's and PSNR within 0.06 dB, with 15 % less memory. Compression level 0 is 1.3 to 2.2 times faster for 0.3 to 0.5 dB less; level 2 is 1.5 to 1.8 times slower for 0.2 to 0.5 dB more.
- **The quality levels barely differ above `Low`.** Against `High`, `Medium` and `Highest` change the file by about 2 % and PSNR by at most 0.15 dB; `Low` saves 7 to 8 % for up to 0.6 dB, and `Thumb` 21 to 23 % for 0.5 to 1.6 dB. In eCorpus's presets, size comes from the maximum size per quality far more than from the quality level. The names stay, so that `optimize` and eCorpus map onto them, and the levels stay eCorpus's, so that outputs can be compared with eCorpus's.
- **UASTC uses level 1 rather than eCorpus's level 2.** basisu v2.50's level 1 gives the PSNR that `ktx` 4.4.2 gets at level 2 (within 0.1 dB) about 20 % faster, for files 1 to 3 % larger. basisu's own level 2 is twice as slow for 0.1 to 0.3 dB more, and level 3 twice as slow again for 0.2 to 0.4 dB more.
- **Zstd 18 is worth its time.** Against level 6 it makes files 11 to 14 % smaller on normal maps and 5 % on ORM, with the same pixels, for up to 25 s on an 8k image.
- At UASTC level 0, basisu's largest mip level is 3 % larger than `ktx` 4.4.2's, although both decode to the same pixels. The cause isn't known yet.

### Measurements

basisu v2.50, 4 threads, against eCorpus's `ktx create` arguments (`--encode basis-lz --qlevel Q --clevel 1`, or `--encode uastc --uastc-quality 2 --zstd 18`), each with `--generate-mipmap`. PSNR is of mip level 0 against the source, decoded to RGBA8 by `ktx extract --transcode rgba8`. The sources are listed in [experiments/fast-textures.md](experiments/fast-textures.md#setup), and every row is in [experiments/ktx-presets.csv](experiments/ktx-presets.csv) (`tools/experiments/textures/bench.sh`). Another session was loading the machine: sizes, PSNR and memory are reliable, times only as rough ratios, which is why the color table leaves them out (both encoders took about the same time at compression level 1). Rerun the rows that set a preset on an idle machine before relying on a time.

| Image | Quality (`-q`) | basisu: size | PSNR | peak RSS | eCorpus (`ktx` 4.4.2): size | PSNR | peak RSS |
| --- | --- | --- | --- | --- | --- | --- | --- |
| color2k | Thumb (128) | 0.44 MB | 41.04 dB | 219 MiB | 0.44 MB | 41.03 dB | 266 MiB |
| color2k | Low (180) | 0.52 MB | 41.51 dB | 221 MiB | 0.52 MB | 41.56 dB | 263 MiB |
| color2k | Medium (220) | 0.55 MB | 41.55 dB | 220 MiB | 0.55 MB | 41.61 dB | 267 MiB |
| color2k | High (240) | 0.56 MB | 41.57 dB | 219 MiB | 0.56 MB | 41.63 dB | 263 MiB |
| color2k | Highest (250) | 0.56 MB | 41.58 dB | 224 MiB | 0.56 MB | 41.64 dB | 264 MiB |
| color4k | Thumb (128) | 1.98 MB | 31.28 dB | 870 MiB | 1.98 MB | 31.27 dB | 1002 MiB |
| color4k | Low (180) | 2.36 MB | 32.24 dB | 873 MiB | 2.35 MB | 32.23 dB | 1005 MiB |
| color4k | Medium (220) | 2.53 MB | 32.69 dB | 881 MiB | 2.52 MB | 32.69 dB | 1010 MiB |
| color4k | High (240) | 2.56 MB | 32.84 dB | 861 MiB | 2.56 MB | 32.83 dB | 992 MiB |
| color4k | Highest (250) | 2.58 MB | 32.91 dB | 877 MiB | 2.58 MB | 32.91 dB | 995 MiB |
| color8k | Thumb (128) | 9.23 MB | 35.27 dB | 3.4 GiB | 9.17 MB | 35.29 dB | 4.0 GiB |
| color8k | Low (180) | 10.84 MB | 36.00 dB | 3.4 GiB | 10.80 MB | 36.04 dB | 4.0 GiB |
| color8k | Medium (220) | 11.52 MB | 36.24 dB | 3.4 GiB | 11.49 MB | 36.28 dB | 4.0 GiB |
| color8k | High (240) | 11.70 MB | 36.32 dB | 3.4 GiB | 11.67 MB | 36.37 dB | 4.0 GiB |
| color8k | Highest (250) | not measured | | | 11.76 MB | 36.41 dB | 4.0 GiB |

| Image | Setting | Time | Size | PSNR | Peak RSS |
| --- | --- | --- | --- | --- | --- |
| normal2k | eCorpus: `ktx` 4.4.2, level 2, Zstd 18 | 4.9 s | 2.06 MB | 48.82 dB | 120 MiB |
| normal2k | **basisu level 1, Zstd 18** | 4.0 s | 2.08 MB | 48.74 dB | 149 MiB |
| normal2k | basisu level 2, Zstd 18 | 8.3 s | 2.15 MB | 49.00 dB | 149 MiB |
| normal2k | basisu level 1, Zstd 6 | 2.2 s | 2.31 MB | 48.74 dB | 108 MiB |
| orm2k | eCorpus: `ktx` 4.4.2, level 2, Zstd 18 | 5.4 s | 4.27 MB | 42.43 dB | 120 MiB |
| orm2k | **basisu level 1, Zstd 18** | 4.1 s | 4.42 MB | 42.46 dB | 150 MiB |
| orm2k | basisu level 2, Zstd 18 | 10.6 s | 4.42 MB | 42.55 dB | 150 MiB |
| orm2k | basisu level 1, Zstd 6 | 3.2 s | 4.63 MB | 42.46 dB | 107 MiB |
| normal8k | eCorpus: `ktx` 4.4.2, level 2, Zstd 18 | 89.9 s | 64.39 MB | 46.87 dB | 1.8 GiB |
| normal8k | **basisu level 1, Zstd 18** | 70.9 s | 65.65 MB | 46.92 dB | 1.3 GiB |
| normal8k | basisu level 2, Zstd 18 | 163.7 s | 67.50 MB | 47.04 dB | 1.3 GiB |
| normal8k | basisu level 1, Zstd 6 | 46.5 s | 73.69 MB | 46.92 dB | 1.3 GiB |

### No fast mode

`ktx` has no fast mode for now. A fast preset produces a temporary model, about 80 % as good, while the final asset is being made, and is only worth having if it is about an order of magnitude faster than the default. No basisu setting at full size gets there: the only candidates encode a smaller image. Whether one of them, or a downscaled WebP, AVIF or JPEG instead, should make fast textures is the experiment in [experiments/fast-textures.md](experiments/fast-textures.md), which holds the measurements so far.

## Report

Per image: the source's name, format and size, the output's index, name and size, codec, transfer function, whether alpha was kept, bytes before and after, and the time. Totals: images encoded and left as they are, bytes before and after, and the time; then the files written. Text by default, JSON with `--json`.

In a chain, each encoded image adds an `imageEncoding` round to `lossy` ([chaining.md](chaining.md)): `{image, format: "image/ktx2", codec, qualityLevel}` for ETC1S, `uastcLevel` instead for UASTC. An earlier round, such as the one `import` records for a JPEG source, makes it warn `repeated-lossy-image-encoding`.

## Validation

There's no reference output to compare against ([validation.md](validation.md)). ctest (`test/ktx/ktx_test.cpp`) decodes the output with basisu's transcoder, which comes with the vendored encoder, and checks, on the `textures`, `alt-sources` and `occupancy` fixtures:

- the codec, transfer function, primaries and alpha of each file, against the slot table above;
- a full mip chain, and sides that are multiples of 4: `alpha.png`, 30×18, becomes 32×20;
- PSNR of mip level 0 against the decoded source of at least 25 dB. The fixtures are 16×16 patterns with hard edges, so this catches broken output (wrong channels, transfer function or orientation) rather than small losses;
- one KTX2 image for an image two textures share, and each texture with its KTX2 source only;
- what is left as it is: the KTX2 texture, the image used as color and as data, small images with `--min-size`; the WebP-only texture is encoded where the build decodes WebP;
- `--max-size`;
- in a chain (`test/commands/chain_test.cpp`), one round per image and the warning for the JPEG `import` recorded.

The fixture `textures` holds the image cases: alpha used, unused (all 255) and ignored (`OPAQUE`), a 30×18 size, a JPEG, a normal map, an ORM image in two data slots, a gray occlusion image, one image behind two color textures, and a 16-bit PNG.

On other models: `ktx validate --gltf-basisu` on every KTX2 file, and `node tools/validate.mjs OUT --against IN`, whose validator doesn't know `KHR_texture_basisu` ([validation.md](validation.md#known-validator-differences)).
