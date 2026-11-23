# uv

`meshtinker uv` packs the UV charts of each material more tightly and bakes the material's textures into the new layout, so that fewer texels are wasted. `inspect` measures the waste as texture occupancy (README). Decoding, resizing, memory and threads are shared with other commands and described in [images.md](images.md). The prototype is `../mesh-pipeline/src/objatlas`, which re-charts one tile with [xatlas](https://github.com/jpcy/xatlas) and resamples its textures.

```bash
meshtinker uv in.gltf out/
meshtinker uv in.gltf out/ --max-size 4096
```

## Order in a pipeline

`uv` runs before `ktx`. It doesn't decode KTX2: a material with a KTX2 texture is left as it is, with the warning `image-already-compressed`, whose message says to run `uv` first ([images.md](images.md#formats)). WebP and AVIF images are baked like PNG and JPEG where the build decodes them; a texture whose only source is a WebP or AVIF extension is left as it is (`image-format-unsupported`). `uv` runs best after `simplify`, on a mesh that xatlas packs in reasonable time ([Time and memory](#time-and-memory)).

## Repack, don't re-chart

`uv` keeps the existing charts and only moves them:

1. A chart is a set of triangles connected in UV space. xatlas finds them (`AddUvMesh`, `ComputeCharts`) by flood-filling triangles that share a UV value, so a triangle soup, whose triangles share no vertex, still makes charts of connected UVs, and every vertex is in one chart.
2. xatlas packs the charts (`PackCharts`) in texels of the group's largest image ("fine texels", `texelsPerUnit` 1), keeping their scale.
3. Each chart's move is rounded to whole texels of every image of the group: a multiple of each image's divisor, the number of fine texels in one of its texels. Baking is then a copy of texels, with no interpolation: no blur, no loss. ctest samples the old textures at the old UVs and the new ones at the new UVs at points inside every triangle, and finds the same values.

xatlas stretches each chart until its extents are whole texels ("Scale the charts to use the entire texel area available", in `PackCharts`), which no option turns off. Without block alignment the stretch is under a texel, so `uv` takes xatlas's placement and moves each chart unstretched, by the move of its minimum corner. The chart then stays within 1.5 texels of the room xatlas gave it, which the padding absorbs. Block alignment is off: it would stretch charts by up to 4 texels.

Charting again from 3D (xatlas `Generate`, as objatlas does) changes every texel, through a resample. Meshes without UVs are out of scope: they have no texture to keep. For very fragmented charts it may become a later optimization, if measurements show it gains enough over repacking (TODO.md).

Repacking changes only `TEXCOORD_n` values: no vertex is added and positions don't move. The rewritten primitives are written uncompressed, every attribute as float (`gltf::addPlainGeometry`), as for any geometry rewrite ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)). Triangles that have no area in UV space, which xatlas leaves out of every chart, move with a chart that has one of their UVs, else with a chart of another of their vertices. The rest, isolated, each sample one point or line of the source: they move with the chart of the placed vertex nearest their centroid, which keeps those texels when the point is in or near that chart. On the local scans they are 3 to 146 triangles in a model, of 145,000 to a million vertices; above 1,000 in a group, whose brute-force search would be slow, the group is left as it is (`uv-unplaced-vertices`). Vertices no triangle uses keep their UVs.

## Groups

- One atlas per material: every primitive using the material is packed together, across meshes. Instanced meshes are packed once.
- Materials that share an image are packed together, so that the shared image gets one layout.
- Merging the atlases of several materials is not done: it would change the material list, and materials differ in factors, alpha mode and double-sidedness.

Each slot of the material (base color, normal, ORM, emissive...) is baked with the same layout. Slots of different sizes keep their ratio: a 4k normal map under an 8k base color gives an atlas half the size of the base color's. That needs each image to be a whole fraction of the largest in both directions, and at most 4 times smaller, since moves are rounded to multiples of that ratio: a group whose images aren't is left as it is (`uv-image-sizes`). Small images (up to 256²) whose texels are all within 4 levels of each other, such as the flat 16² metallic-roughness map of a scan whose other maps are 2k and 4k, read the same at any UV: they keep their file and stay out of the layout. JPEG noise on such flat images reaches 3 levels on the local scans.

## Atlas size

- The atlas keeps the source's texel density, so its size follows from the charts' area.
- Charts get xatlas's bilinear border and a padding of 3 texels of the smallest image, and the atlas a margin of 2 such texels on every side: xatlas places some charts against its edge. At padding 3, ctest's sampled comparison finds every triangle's texels intact. Around each triangle, the texels within 1.5 texels of it are copied, since bilinear filtering at a point of the triangle reads texels up to √2 away. The rest is filled by push-pull from the copied texels, as objatlas does, so that mipmaps don't mix neighbouring charts with black.
- Each side is rounded up to a multiple of 4, ready for `ktx`.
- A group whose images wouldn't lose at least 10 % of their texels is left as it is, images and UVs untouched (`no-gain` in the report, without a warning).
- `--max-size N` then scales the baked images down if needed ([images.md](images.md#resizing)), which resamples them.

The baked images are PNG, at a fast compression level ([images.md](images.md#writing)), named after their source: `wall.jpg` becomes `wall.png`, and `wall.png` becomes `wall_1.png`. A PNG is no lossy round, so `uv` records none in a chain.

## Normal maps and rotation

Rotating or mirroring a chart turns its tangent space, which makes a tangent-space normal map and any `TANGENT` attribute wrong. Charts are never rotated (xatlas `rotateCharts` and `rotateChartsToAxis` off), nor mirrored, which xatlas doesn't do anyway. Rotation by 90° steps would keep baking a copy of texels for materials without a normal map, and rotating each chart's normals with it, and recomputing `TANGENT`, would allow it with one. Both can come later if measurements show they are worth the code (TODO.md).

## Left as they are

Each of these is a counted warning, and the material is left as it is:

- a texture on another texture coordinate set than the material's others (`uv-mixed-texcoords`);
- `KHR_texture_transform` (`uv-texture-transform`);
- UVs outside [0, 1] on a `REPEAT` or `MIRRORED_REPEAT` sampler, which is tiling on purpose (`uv-texture-repeats`);
- morph targets with `TEXCOORD_n` deltas (`uv-morphed-texcoords`);
- images it can't decode: KTX2 (`image-already-compressed`, run `uv` before `ktx`), textures whose only source is a WebP or AVIF extension, and formats the build doesn't decode (`image-format-unsupported`), missing files (`image-missing`), and corrupt ones (`image-decode-failed`) ([images.md](images.md#formats));
- images that aren't whole multiples of each other in size (`uv-image-sizes`);
- points, lines, or a primitive without the texture coordinate set its material uses (`uv-unreadable-geometry`);
- a UV mesh xatlas refuses, or vertices left in no chart (`uv-pack-failed`, `uv-unplaced-vertices`).

## Time and memory

xatlas keeps the whole UV mesh of a group on the heap, plus its own per-vertex and per-chart records. This falls under the memory policy of [images.md](images.md#memory-and-threads): low where easy, with the cost measured and documented. The settings come from measuring, on the corpus, how the texels saved grow with packing effort (`tools/experiments/uv/measure.py`, rows in [experiments/uv-collection.csv](experiments/uv-collection.csv)):

| Setting | Groups repacked (of 710, 56 models) | Texels saved | Time, all models | Slowest model |
| --- | --- | --- | --- | --- |
| padding 3 (default) | 257 | 5.9 % | 426 s | 38 s, 860 MiB |
| padding 2 | 300 | 5.5 % | 386 s | 28 s |
| brute-force placement | not run on the collection | | | 361 s instead of 5 s on a 64-chart 4k scan; over 10 minutes instead of 1.4 s on a 1,287-chart one |

- **Placement:** xatlas's default random placement. Brute force is 75 to over 400 times slower, which no gain justifies.
- **Padding:** 3, the smallest that ctest's sampled comparison finds safe. Padding 2 repacks a few more groups but isn't checked.
- **Who gains:** 13 of the 55 single-scan models gain 10 % or more (12 % to 61 %); the median is 4 %, and a quarter would grow, their atlases already tight. The 634-material campus scan repacks 244 of its groups and saves 18 % of its texels. The 10 % threshold stays as the default: 13 more models gain between 5 and 10 %, which a lower threshold would repack for a small saving each.
- **Size:** no size makes `uv` too slow on the corpus: the 44-million-face scan takes 40 s and peaks at 2.9 GB (about 65 bytes a face, mostly xatlas's), and its 4k atlas wouldn't gain. `optimize` doesn't need to skip `uv` by size up to there; memory is what grows, and it can skip `uv` when the faces times 65 bytes don't fit.
- Charts don't decide the time: 140,000 charts pack in 18 s. Decoding, baking and writing 8k images do.

## Report

Per group: its materials, status (`repacked`, `no-gain`, or `skipped` with the warning's code as `reason`), primitives, charts, each image's size before and after with its output's name, texels before and after (for `no-gain`, the atlas it would have had), occupancy before and after (the texels the charts cover, over all texels), and the time. Totals over the scene, then the files written.

## Validation

There's no reference output to compare against ([validation.md](validation.md)). ctest (`test/uv/uv_test.cpp`):

- **Sampled comparison**, as objatlas measures it: at 8 random points in every triangle, the source texture at the old UV against the new texture at the new UV, with bilinear filtering, on `scan-tiles`, `scan-meshes`, `atlas`, `occupancy` and `udim`. The largest difference must be under 0.01 of 255: whole-texel moves read the same texels under the same weights.
- The `atlas` fixture: three charts spread over a 128×128 base color and a 64×64 normal map, and a second material sharing the base color, packed together, with the images' 2:1 ratio kept.
- What is left as it is, on `occupancy` (texture transform, tiling, two texture coordinate sets, an image already full) and `alt-sources` (KTX2, WebP).
- `--max-size`.
- In a chain (`test/commands/chain_test.cpp`): a stage with no lossy round, then `ktx`, then `uv` warning on the KTX2 images.

On other models: `node tools/validate.mjs OUT --against IN`, `inspect` occupancy before and after, and `tools/experiments/uv/measure.py` for texels, time and memory.
