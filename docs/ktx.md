# ktx

`meshtinker ktx` encodes the images of the scene as KTX2 with Basis Universal and references them through `KHR_texture_basisu`. It replaces glTF-Transform's `toktx` calls in eCorpus's `optimizeGlb.ts`. This is the design. Decoding, resizing, memory and threads are shared with other commands and described in [images.md](images.md). The benchmark that sets the presets is under [Presets](#presets).

```bash
meshtinker ktx in.gltf out/                    # default quality
meshtinker ktx in.gltf out/ --quality Low --max-size 2048 --fast
```

## Encoder

basis_universal (BinomialLLC, Apache-2.0) is vendored and compiled in. Its encoder library writes KTX2 itself, including Zstandard supercompression for UASTC (with Zstd vendored inside it) and mipmap generation.

The two alternatives were rejected:
- **Calling the `ktx` program**, as eCorpus does through glTF-Transform. It is a runtime dependency that users must install, and its 4.x and 5.x versions differ in flags.
- **Vendoring libktx.** It wraps the same basisu encoder, and its CMake tree is much larger. Only its KTX2 container code would be on top of what we need.

`ktx validate` (KTX-Software) is used in validation, never at runtime.

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
- **Channels:** an image whose alpha is unused ([images.md](images.md#channel-analysis)) is written as RGB. Single-channel `occlusionTexture` images are written as Red, without sRGB, as the spec requires for Red and Red-Green.
- Images are always written with a full mip chain: the spec says an image SHOULD have one when its sampler minifies with mipmaps, or has no sampler.

## Textures and sources

- An encoded texture gets `extensions.KHR_texture_basisu.source`, pointing to the new image (`mimeType: image/ktx2`).
- **The core `source` is removed**, and `KHR_texture_basisu` goes into both `extensionsUsed` and `extensionsRequired`, as the spec says for files without fallback. This is what eCorpus does, and its viewer reads KTX2. A fallback would make the output larger for readers that eCorpus doesn't need to serve.
- The texture's other extension sources (`EXT_texture_webp`, `EXT_texture_avif`) are removed too, so that an encoded texture has its KTX2 source only. A renderer that reads KTX2 picks it anyway, and the others would only cost space. This is a stated exception to "don't strip": `ktx` replaces a texture's sources, it doesn't add one.
- `gltf::prune` then removes the images nothing uses any more.
- A texture whose only decodable source is missing (WebP or AVIF only) is left as it is, with `image-format-unsupported`.
- A texture already in KTX2 is kept and not re-encoded.

## Presets

The aim is few options:

- `--quality <Highest|High|Medium|Low|Thumb>` (default `High`), the same names as `optimize`, so that it passes its own straight through.
- `--fast`, only if it can be about ten times faster than the default ([below](#the-fast-preset)).
- `--max-size N` ([images.md](images.md#resizing)).

eCorpus's settings are the baseline:
- ETC1S `qlevel` by quality: Thumb 128, Low 180, Medium 220, High 240, Highest 250, `clevel` 1.
- UASTC at level 2, Zstd 18, no RDO, at every quality.
- `maxSize` by quality: 512, 1024, 2048, 4096, 8192.

`--max-size` stays explicit: `optimize` maps its quality to a size, as eCorpus does, but `ktx` alone doesn't resize unless asked.

The benchmark (below) fills in the settings table for each quality, and decides what `--fast` changes (basisu's `-effort` and `-quality`, the UASTC level, the Zstd level, RDO), with the time, size and PSNR of each against eCorpus's settings.

### The fast preset

A fast preset exists to produce a temporary model, about 80 % as good, while the final asset is being made. It is only worth having if it is about an order of magnitude faster than the default: a speedup of 1.5 or 2 is a slower default, not a fast preset. If no setting gets there, `ktx` has no fast mode, and fast pipelines skip KTX2 for something else, such as a quick JPEG placeholder command.

Only a smaller image gets there: no encoder setting at full size does. These are measured with basisu v2.50, 4 threads, on two scan color maps. PSNR is against the full-size source, after decoding to RGBA8 and scaling back up, so it includes the loss from resizing. Times were taken while another session was loading the machine: compare them as ratios within one image, not across tables.

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

- ETC1S settings at full size gain 2 at best. ETC1S's codebook search is the cost, and only fewer texels cut it.
- UASTC level 0 at half size is 16 to 43 times faster than the default. Its PSNR is 1.9 dB below the default on the 4k image and 0.3 dB above it on the 8k one. Its files are about 1.8 times larger. On the GPU it takes half the memory of the full-size ETC1S image: BC7 or ASTC at 8 bits per texel over a quarter of the texels, against BC1 or ETC1 at 4.
- ETC1S at half size is 6 to 13 times faster with files 4 times smaller, at 2.7 to 3.9 dB below the default.
- An 8k ETC1S encode peaks at 3.5 GiB, about 13 times the decoded RGBA image. UASTC needs about a third of that.

The candidate for `--fast` is UASTC level 0, Zstd 1, at half the size `--quality` and `--max-size` would give. Whether it is "80 % as good" is not settled (TODO.md).

<!-- benchmark results go here -->

## Report

Per image: source format and size, codec, output size in texels, bytes before and after, encoding time. Totals: bytes before and after, time. Text by default, JSON with `--json`.

## Validation

There's no reference output to compare against ([validation.md](validation.md)). The checks are:

- PSNR of each image against its decoded source, at least a threshold per quality, measured on the corpus. Tests decode KTX2 with basisu's transcoder, which comes with the vendored encoder.
- `ktx validate` on every KTX2 file, and the glTF validator on the model.
- A fixture with:
  - a color image with alpha and one without;
  - a normal map;
  - an ORM image;
  - an occlusion-only image;
  - a size that isn't a multiple of 4;
  - a JPEG;
  - a WebP-only texture;
  - an image already in KTX2;
  - an image shared by a color slot and a data slot;
  - an image shared by two color slots.

