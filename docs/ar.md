# AR exports

What meshtinker writes for augmented reality, and why. Nothing here is implemented yet: the items are in TODO.md, and the choices still open are under its "Pending decisions".

## Targets

Voyager, the viewer eCorpus serves, opens AR in one of two ways (`CVARManager.ts`):

| Device | Viewer | eCorpus derivative | File |
| --- | --- | --- | --- |
| Android, and any browser with WebXR `immersive-ar` | Voyager itself, three.js in a WebXR session | `Web3D` / `AR` | GLB |
| iOS | AR Quick Look, opened by a `rel="ar"` link | `iOSApp3D` / `AR` | USDZ |

Voyager also falls back to Scene Viewer (`App3D` / `AR`) on Android without WebXR. It is not a target: in practice, devices that aren't iOS have WebXR or no AR at all. Without an `App3D` derivative, Voyager offers no AR on those devices, which is what they get today.

Both files come from the same chain, and differ only in how they encode the same geometry and the same images ([Making the two match](#making-the-two-match)). The WebXR model is rendered by the same three.js as the desktop viewer, so its quality is checked like any other `optimize` output. Quick Look renders with RealityKit, whose lighting differs: the two can match in content, not in pixels.

## What each output uses

| | GLB for WebXR | USDZ for Quick Look |
| --- | --- | --- |
| Container | GLB | zip, stored (no compression), each file on a 64-byte boundary, the USD layer first |
| Scene description | glTF JSON | USD text layer (`.usda`) |
| Geometry | meshopt (`compress`) | float32 points, normals and texture coordinates, 32-bit indices: USD has no compressed geometry |
| Color textures (base color, emissive) | KTX2 ETC1S | JPEG, or PNG when the image's alpha is used |
| Data textures (normal, occlusion, metallic-roughness) | KTX2 UASTC | JPEG, or PNG when the image's alpha is used |
| Image sides | the AR quality's, the same in both | the same |
| Images already small enough | encoded to KTX2 like the others | PNG and JPEG copied as they are |
| GPU memory per texel | 0.5 to 1 byte, transcoded to ASTC or ETC2 | 4 bytes (RGBA8), counted as such |
| Unknown material extensions | kept, as everywhere | dropped, with a warning |

### GLB for WebXR

The steps and encodings are those of `optimize`'s slow preset ([optimize.md](optimize.md)), at a lower quality:

- **meshopt geometry.** Voyager registers three.js's `MeshoptDecoder`, and its WASM decoder is fast enough on phones. Draco is decoded too, and `compress` keeps Draco primitives as they are ([compress.md](compress.md)), but meshtinker never writes Draco.
- **KTX2 textures**, with `ktx`'s rules ([ktx.md](ktx.md)): ETC1S for color, UASTC for data. three.js's `KTX2Loader` transcodes them to a format the GPU samples directly (ASTC or ETC2 on Android GPUs), so they take a quarter of their RGBA8 memory or less. This matters most in AR, where the camera feed and the AR runtime share the phone's memory with the model. Not AVIF, which the fast preset writes: it unpacks to RGBA8 on the GPU and decodes on the phone's CPU.
- **Sizes**: Medium's simplification error and 2048 at most on the texture side, as eCorpus uses today. The GPU budget is computed from the USDZ's figures (below), since those are the larger.

### USDZ for Quick Look

Its layout is the OpenUSD usdz specification's ([spec_usdz](https://www.openusd.org/dev/spec_usdz.html)): an uncompressed, unencrypted zip, each file starting on a 64-byte boundary so that it can be mapped in place, the USD layer first, and Zip64 past 4 GiB. The zip writer is ours: stored entries, CRC-32, padding in the local headers' extra field, no library.

- **Text layer (`.usda`).** Writing it is simple, and it streams front to back from the model's mapped buffers. The binary layer (`.usdc`, "Crate") is smaller, about half the size of the text for geometry, but it has no complete published spec, and OpenUSD itself is too large a dependency for these builds. At AR sizes the textures dominate either way: a 100,000-face mesh is about 6 MB of text, while a 2048 JPEG is 1 to 2 MB. Numbers are written with `std::to_chars`, the shortest form that reads back exactly, whatever the locale.
- **Uncompressed geometry.** USD has no counterpart to meshopt or Draco, and the zip is stored. The geometry is therefore the GLB's before `compress`, read in float32.
- **JPEG or PNG images.** The spec also allows EXR and AVIF, but the guides for Quick Look list only PNG and JPEG, so these two until a device shows otherwise. An image is written as JPEG unless its alpha is used ([channel analysis](images.md#channel-analysis)), then as PNG, normal maps included: the AR experiment checks that JPEG's blocks don't show in them under Quick Look's lighting. A PNG or JPEG image already at its side is copied as it is. WebP and AVIF are decoded and encoded again, and KTX2 too, once `image::decode` can decode it (TODO.md). JPEG comes from the vendored `stb_image_write`.
- **RGBA8 on the GPU.** Quick Look doesn't document how it uploads PNG and JPEG, so the budget counts 4 bytes per texel, plus a third for mipmaps: 21 MiB for a 2048 texture.
- **Units.** glTF's unit is the meter, so the layer says `metersPerUnit = 1` and `upAxis = "Y"`. Quick Look shows models at their real size, so a model authored in another unit is shown at the wrong size (see Pending decisions).

The mapping follows three.js's `USDZExporter`:

| glTF | USD |
| --- | --- |
| node | `Xform` with `xformOp:transform`. glTF's 16 column-major values are USD's row-vector `matrix4d` in the same order |
| primitive | `Mesh`: `faceVertexCounts` all 3, `faceVertexIndices` the glTF indices, `points`, and `normals` and `primvars:st` with `vertex` interpolation. glTF vertices are already indexed per vertex, so nothing is unwelded. `v` becomes `1 − v`: USD's origin is the bottom left |
| `TEXCOORD_n` | `primvars:st`, `st1`..., each `UsdUVTexture` reading its own through a `UsdPrimvarReader_float2` |
| base color | `UsdPreviewSurface` `diffuseColor` from the texture's RGB, `opacity` from its alpha. The factor becomes the texture's `scale` |
| `alphaMode` | `MASK`: `opacityThreshold` from `alphaCutoff`. `BLEND`: `opacity`. `OPAQUE`: neither |
| metallic-roughness | one `UsdUVTexture`, `outputs:b` to `metallic` and `outputs:g` to `roughness`: no channel splitting |
| normal map | `outputs:rgb` with `scale (2, 2, 2, 1)` and `bias (−1, −1, −1, 0)`, `sourceColorSpace = "raw"` |
| occlusion | `outputs:r` to `occlusion` |
| emissive | `emissiveColor`, the factor as `scale` |
| sampler wrap modes | `wrapS`, `wrapT` (`repeat`, `mirror`, `clamp`). USD has no filter modes: they are dropped |
| `KHR_texture_transform` | `UsdTransform2d` between the primvar reader and the texture |
| `doubleSided` | `doubleSided` on the `Mesh` |

What USD's preview material can't say is dropped, with one counted warning per kind ([Real files are messy](../AGENTS.md#constraints)): vertex colors (`UsdPreviewSurface` has no input for them), Khronos material extensions other than texture transforms, animations and skins. `KHR_materials_unlit` has no counterpart in `UsdPreviewSurface`, and photogrammetry is often published unlit, with its lighting baked into the texture. An unlit material is written emissive: `diffuseColor` black, `emissiveColor` the base color, roughness 1 and metallic 0. Quick Look then adds no shading of its own, only a faint specular, where a lit material would light the scan a second time.

## Making the two match

`export` writes a chain's stage, and `--stage` picks one other than the last ([chaining.md](chaining.md)), so one linear chain gives both files. Both branch from the stage before the texture step, and each encodes the images once, at the same side:

```
import → normals → weld → dedup → simplify → (uv)
                                               ├── export --stage N --max-size S  → model.usdz
                                               └── ktx --max-size S → compress → export → model.glb
```

- **The USDZ export sizes its images itself.** It has to decode and encode every image that isn't PNG or JPEG anyway, so it is a third user of the `texture` namespace, beside `ktx` and `avif`: the same slot rules, resampler and threads, with a JPEG and PNG codec, and the same `--max-size`. A PNG or JPEG image already within the side is copied as it is. Given the same side, `ktx` and the export resize alike, so the two outputs' images have the same sizes.
- **No resizing stage of its own.** A transform that resized before `ktx` would add nothing that `ktx`, `avif` and `uv` don't do with `--max-size` already, and its stage would have to hold PNG, since a JPEG there would give the GLB a second lossy round (JPEG, then KTX2), with `repeated-lossy-image-encoding`.
- **The two outputs share** the geometry, the images' sides, the materials and the node tree. They differ in the geometry's quantization (meshopt's, on the GLB only), the image codecs, and what USD can't say.
- **`optimize --quality AR in.glb out.glb --usdz out.usdz`** runs this chain and writes both files. It has Medium's error and image side, and its GPU budget is computed with 4 bytes per texel, the USDZ's, whatever the GLB's KTX2 needs.

## Checking

- **The GLB:** as any `optimize` output ([validation.md](validation.md)), rendered with `tools/viewer` and scored with SSIMULACRA2 against the input.
- **The USDZ:** OpenUSD's `usdchecker --arkit`, which applies the rules for ARKit. Blender's USD importer, rendered beside the GLB, checks the material mapping (not Quick Look's renderer). Then a look on an iPhone, which only a person can do.
- **Sizes:** an experiment on the survey's models, at the AR quality, records each output's bytes and GPU memory, and the frame rate on a phone where it can be measured. It settles the open sizes below.

Apple doesn't publish hard limits. A DTS engineer says there is no contract on texture size, and the common guidance is about 100,000 triangles and textures of 2048 at most ([Apple developer forums](https://developer.apple.com/forums/thread/693867), [Sketchfab](https://help.sketchfab.com/en/articles/16152669-gltf-glb-and-usdz)). `simplify` is error-only, so the AR quality has no face count of its own until the experiment shows it needs one.
