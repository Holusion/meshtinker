# ktx

`meshtinker ktx` encodes the images of the scene as KTX2 with Basis Universal and references them through `KHR_texture_basisu`. It replaces glTF-Transform's `toktx` calls in eCorpus's `optimizeGlb.ts`. This is the design. Decoding, resizing, memory and threads are shared with other commands and described in [images.md](images.md). Questions still open are at the end, and the benchmark that sets the presets is under [Presets](#presets).

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
- **An image used only by slots the command doesn't know** (an unknown extension) is left as it is, with the warning `image-slot-unknown`. Guessing would risk the wrong transfer function, and three.js's `KTX2Loader` follows the file's transfer function.
- **Channels:** an image whose alpha is unused ([images.md](images.md#channel-analysis)) is written as RGB. Single-channel `occlusionTexture` images are written as Red, without sRGB, as the spec requires for Red and Red-Green.
- Images are always written with a full mip chain: the spec says an image SHOULD have one when its sampler minifies with mipmaps, or has no sampler.

## Textures and sources

- An encoded texture gets `extensions.KHR_texture_basisu.source`, pointing to the new image (`mimeType: image/ktx2`).
- **The core `source` is removed**, and `KHR_texture_basisu` goes into both `extensionsUsed` and `extensionsRequired`, as the spec says for files without fallback. This is what eCorpus does, and its viewer reads KTX2. Keeping a fallback is open (question 1).
- The texture's other extension sources (`EXT_texture_webp`, `EXT_texture_avif`) are removed too (question 2).
- `gltf::prune` then removes the images nothing uses any more.
- A texture whose only decodable source is missing (WebP or AVIF only) is left as it is, with `image-format-unsupported`.
- A texture already in KTX2 is kept and not re-encoded.

## Presets

The aim is few options:

- `--quality <Highest|High|Medium|Low|Thumb>` (default `High`), the same names as `optimize`, so that it passes its own straight through.
- `--fast`, which must be at least twice as fast as the default for a small loss in quality.
- `--max-size N` ([images.md](images.md#resizing)).

eCorpus's settings are the baseline:
- ETC1S `qlevel` by quality: Thumb 128, Low 180, Medium 220, High 240, Highest 250, `clevel` 1.
- UASTC at level 2, Zstd 18, no RDO, at every quality.
- `maxSize` by quality: 512, 1024, 2048, 4096, 8192.

`--max-size` stays explicit: `optimize` maps its quality to a size, as eCorpus does, but `ktx` alone doesn't resize unless asked.

The benchmark (below) fills in the settings table for each quality, and decides what `--fast` changes (basisu's `-effort` and `-quality`, the UASTC level, the Zstd level, RDO), with the time, size and PSNR of each against eCorpus's settings.

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

## Open questions

1. **Fallback.** Remove the core `source`, as above and as eCorpus does, or keep the PNG/JPEG as fallback (larger output, readable without KTX2 support)? Proposal: remove it. A `--keep-fallback` option can come if a user needs it.
2. **WebP and AVIF sources on an encoded texture.** Remove them (proposal: the renderer would pick KTX2 anyway, and they cost space), or keep them, by the "don't strip" rule?
3. **Unknown slots and color/data conflicts** are left as they are, with a warning. Alternatively, an image shared by a color slot and a data slot could be written twice, once as each.
4. **Default quality.** `High` is proposed: it matches eCorpus's default derivative.
