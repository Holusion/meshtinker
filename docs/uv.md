# uv

`meshtinker uv` packs the UV charts of each material more tightly and bakes the material's textures into the new layout, so that fewer texels are wasted. `inspect` measures the waste as texture occupancy (README). This is the design. Decoding, resizing, memory and threads are shared with other commands and described in [images.md](images.md). The prototype is `../mesh-pipeline/src/objatlas`, which re-charts one tile with [xatlas](https://github.com/jpcy/xatlas) and resamples its textures.

```bash
meshtinker uv in.gltf out/
meshtinker uv in.gltf out/ --max-size 4096
```

## Order in a pipeline

`uv` runs before `ktx`. It doesn't decode KTX2: a material with a KTX2 texture is left as it is, with the warning `image-already-compressed`, whose message says to run `uv` first ([images.md](images.md#formats)). WebP and AVIF are left as they are too (`image-format-unsupported`). `uv` runs best after `simplify`, on a mesh that xatlas packs in reasonable time ([Time and memory](#time-and-memory)).

## Repack, don't re-chart

`uv` keeps the existing charts and only moves them:

1. A chart is a set of triangles connected in UV space. Vertices are joined by equal position and UV before charts are found, so that a triangle soup doesn't become one chart per triangle.
2. xatlas packs the charts (`AddUvMesh`, then `PackCharts`), keeping their scale relative to the source texels.
3. Each chart's move is rounded to whole texels of the source texture. Baking is then a copy of texels, with no interpolation: no blur, no loss.

Charting again from 3D (xatlas `Generate`, as objatlas does) changes every texel, through a resample. Meshes without UVs are out of scope: they have no texture to keep. For very fragmented charts it may become a later optimization, if measurements show it gains enough over repacking (TODO.md).

Repacking changes only `TEXCOORD_n` values: no vertex is added and positions don't move. The rewritten primitives are written uncompressed, as for any geometry rewrite ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)).

## Groups

- One atlas per material: every primitive using the material is packed together, across meshes. Instanced meshes are packed once.
- Materials that share an image are packed together, so that the shared image gets one layout.
- Merging the atlases of several materials is not done: it would change the material list, and materials differ in factors, alpha mode and double-sidedness.

Each slot of the material (base color, normal, ORM, emissive...) is baked with the same layout. Slots of different sizes keep their ratio: a 4k normal map under an 8k base color gives an atlas half the size of the base color's.

## Atlas size

- The atlas keeps the source's texel density, so its size follows from the charts' area.
- Each side is rounded up to a multiple of 4, ready for `ktx` (xatlas `blockAlign`).
- `--max-size N` then scales it down if needed ([images.md](images.md#resizing)).
- A material whose atlas wouldn't meaningfully improve on its source images is left as it is, with its images and UVs untouched. The threshold (fewer texels by some fraction, at first 10 %) is set by the measurements in [Time and memory](#time-and-memory).
- Charts get a padding of a few texels, filled by push-pull from the chart edges, as objatlas does, so that mipmaps don't mix neighbouring charts.

The baked images are PNG, at a fast compression level ([images.md](images.md#writing)).

## Normal maps and rotation

Rotating or mirroring a chart turns its tangent space, which makes a tangent-space normal map and any `TANGENT` attribute wrong. Rotation is turned off for materials with a normal map, and so is mirroring, which xatlas doesn't do anyway. Other materials allow rotation by 90° steps, which keeps baking a copy of texels. Rotating each chart's normals with it, and recomputing `TANGENT`, would pack tighter, and can come later if the measurements show it is worth the code.

## Left as they are

Each of these is a counted warning, and the material is left as it is:

- a texture on another texture coordinate set than the material's others (`uv-mixed-texcoords`);
- `KHR_texture_transform` (`uv-texture-transform`);
- UVs outside [0, 1] on a `REPEAT` or `MIRRORED_REPEAT` sampler, which is tiling on purpose (`uv-texture-repeats`);
- morph targets with `TEXCOORD_n` deltas (`uv-morphed-texcoords`);
- images it can't decode ([images.md](images.md#formats)).

## Time and memory

xatlas keeps the whole UV mesh of a group on the heap, plus its own per-vertex and per-chart records. Charting tens of millions of faces wouldn't finish in reasonable time either. This falls under the memory policy of [images.md](images.md#memory-and-threads): low where easy, with the cost measured and documented. There is no target time set in advance. The settings come from measuring, on the corpus, how the texels saved grow with xatlas's packing effort (`bruteForce`, packing resolution, chart count): `uv` uses the point past which more effort saves little. The same measurements set the size above which `optimize` skips `uv`, and the threshold below which a material is left as it is.

## Report

Per material: charts, texels before and after, occupancy before and after, time. Totals over the scene.

## Validation

There's no reference output to compare against ([validation.md](validation.md)). The checks are:

- **Sampled comparison:** at random points on the surface, the source texture at the old UV against the new texture at the new UV, as objatlas measures it. With whole-texel moves the difference should be zero away from chart edges.
- `inspect` occupancy before and after.
- The glTF validator.
- A fixture with:
  - several charts with gaps between them;
  - a normal map;
  - two materials sharing an image;
  - a tiling `REPEAT` texture;
  - a texture transform;
  - slots of different sizes.

