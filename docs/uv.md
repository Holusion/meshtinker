# uv

`meshtinker uv` packs the UV charts of each material more tightly and bakes the material's textures into the new layout, so that fewer texels are wasted. `inspect` measures the waste as texture occupancy (README). This is the design. Decoding, resizing, memory and threads are shared with other commands and described in [images.md](images.md). Questions still open are at the end. The prototype is `../mesh-pipeline/src/objatlas`, which re-charts one tile with [xatlas](https://github.com/jpcy/xatlas) and resamples its textures.

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

Charting again from 3D (xatlas `Generate`, as objatlas does) changes every texel, through a resample. It is only worth it for meshes without UVs, or with very fragmented charts. It would be a command of its own, `uv-unwrap` (question 2).

Repacking changes only `TEXCOORD_n` values: no vertex is added and positions don't move. The rewritten primitives are written uncompressed, as for any geometry rewrite ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)).

## Groups

- One atlas per material: every primitive using the material is packed together, across meshes. Instanced meshes are packed once.
- Materials that share an image are packed together, so that the shared image gets one layout.
- Merging the atlases of several materials is not done (question 1).

Each slot of the material (base color, normal, ORM, emissive...) is baked with the same layout. Slots of different sizes keep their ratio: a 4k normal map under an 8k base color gives an atlas half the size of the base color's.

## Atlas size

- The atlas keeps the source's texel density, so its size follows from the charts' area.
- Each side is rounded up to a multiple of 4, ready for `ktx` (xatlas `blockAlign`).
- `--max-size N` then scales it down if needed ([images.md](images.md#resizing)).
- A material whose atlas wouldn't have fewer texels than its source images is left as it is (question 5).
- Charts get a padding of a few texels, filled by push-pull from the chart edges, as objatlas does, so that mipmaps don't mix neighbouring charts.

## Normal maps and rotation

Rotating or mirroring a chart turns its tangent space, which makes a tangent-space normal map and any `TANGENT` attribute wrong. Rotation is turned off for materials with a normal map, and so is mirroring, which xatlas doesn't do anyway. Other materials allow rotation by 90° steps, which keeps baking a copy of texels (question 3).

## Left as they are

Each of these is a counted warning, and the material is left as it is:

- a texture on another texture coordinate set than the material's others (`uv-mixed-texcoords`);
- `KHR_texture_transform` (`uv-texture-transform`);
- UVs outside [0, 1] on a `REPEAT` or `MIRRORED_REPEAT` sampler, which is tiling on purpose (`uv-texture-repeats`);
- morph targets with `TEXCOORD_n` deltas (`uv-morphed-texcoords`);
- images it can't decode ([images.md](images.md#formats)).

## Time and memory

xatlas keeps the whole UV mesh of a group on the heap, plus its own per-vertex and per-chart records. Charting tens of millions of faces wouldn't finish in reasonable time either. This falls under the memory policy of [images.md](images.md#memory-and-threads): low where easy, with the cost measured and documented. The target time per million faces, xatlas's packing effort (`bruteForce`, resolution) and the limit above which `optimize` skips `uv` are set by measurement on the corpus.

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

## Open questions

1. **Atlas per material, or merged.** One atlas per material (and materials sharing images) is the design above. Merging compatible materials (same factors, alpha mode, double-sidedness) into one atlas would cut draw calls on scans split into texture pages, but changes the material list. Proposal: per material now, merging as a later command.
2. **Re-charting.** Is `uv-unwrap` (re-chart from 3D, then resample) wanted, and when: meshes without UVs, very fragmented charts?
3. **Rotation with normal maps.** Rotation is off for materials with a normal map (above). The alternative is to rotate each chart's normal XY by its rotation and recompute `TANGENT`: tighter packing, more code.
4. **Baked image format.** PNG is lossless, but large and slow to write with stb on big images. JPEG is smaller but adds a lossy round. Proposal: PNG at a fast compression level, since `ktx` usually follows. If `uv`'s output must be shippable without `ktx`, JPEG for color slots whose source was JPEG.
5. **No gain.** Leave a material as it is when the atlas wouldn't have fewer texels (proposal), or always repack and report?
6. **Time target.** What run time is acceptable for `uv` on a simplified scan (for instance 1 million faces): seconds, a minute?
