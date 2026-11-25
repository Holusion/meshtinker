# normals

How `meshtinker normals` writes normals, and why. The code is `src/commands/normals/`, on the shared geometry editing of [gltf-io.md](gltf-io.md#editing). The groundwork, a survey of how 136 models store their normals, is [experiments/normals.md](experiments/normals.md).

## Which primitives get normals

The rule is AGENTS.md's: **normals default to smooth**. Missing normals and flat ones are presumed to be a mistake made upstream, unless the data shows the hard edges are meant.

- **No `NORMAL`.** The spec says a viewer renders such a primitive flat. Most local scans are like this: 40 of 70 files. Primitives with a normal map but no `NORMAL` are included: the spec makes viewers ignore their tangents, and eCorpus's viewer already computes smooth normals for them.
- **Flat normals:** the three normals of every triangle are equal, −0 and 0 alike, which draws one normal per face. The campus scan and `collection-16` store theirs this way. Close isn't enough: dense scans have a third of their triangles within 1° of their face normal, smoothly (case 8 of the survey). `--keep-flat` leaves these as they are.

Everything else is copied as it is:

- **Authored normals**, smooth or split along hard edges, are kept, and so are flat ones with `--keep-flat` (`kept` in the report).
- **Unlit primitives** (`KHR_materials_unlit`) are left without normals: no viewer shades them, so normals would only add bytes (`unlit`).
- **Points and lines** aren't shaded by face, and aren't counted.
- **Primitives with morph targets** are copied with a warning (`morph-targets-kept`). A target that moves positions would need its own normal displacements, which are not computed.
- **Primitives that can't be read** are copied with a warning (`normals-not-written`).

## How normals are computed

1. **Corners are grouped by position**, compared bit for bit with −0 counting as 0, not by vertex. Vertices duplicated at a UV seam therefore get the same normal, and the seam leaves no crease. eCorpus's viewer, Voyager, sums by vertex instead, with three.js's `computeVertexNormals`, and creases every UV seam of a scan without normals.
2. **Primitives in one space are grouped together**: those of one mesh, and those of meshes drawn by nodes with the same world matrices. The tiles of a scan, as primitives or as meshes, then share normals along their borders, and show no seam. A mesh drawn by an instancing node, or by no node, is a group of its own. Positions are compared as stored, as in `simplify`'s shared borders ([simplify.md](simplify.md#borders-between-primitives)).
3. **Around each position, faces that share an edge join** when their normals are at most the crease angle apart. Each set of joined faces gets one normal: the sum of its faces' unit normals, each weighted by its corner's angle, as Blender's smooth normals are. A vertex whose corners fall in two sets is split in two. Face normals follow the winding (counter-clockwise is the front, as the spec says).
4. **Every corner of a set gets the same value, bit for bit.** `weld` compares normals bit for bit, so vertices that differ only by their former flat normals merge after `normals`.
5. **Corners of zero-area faces** take the normal of the heaviest set at their position, so that they merge too. Where the faces of a set cancel out (two faces back to back, possible only without creases), the set takes one of its face normals; a position that only zero-area faces draw gets +Y.

Rewritten primitives are written as every command that rewrites geometry writes them ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)): float32 attributes, indexed triangle lists, vertices in first-use order, unused vertices dropped. A primitive that shares vertex data with another gets a copy of its own.

**`TANGENT` is dropped.** Stored tangents were built for the normals they came with, which are gone. Without them, viewers compute tangents from the new normals and the texture coordinates, as the spec asks. Per-face tangents would also keep `weld` from merging anything.

## Creases

Edges whose faces meet at more than `--crease-angle` stay hard. The default is 30°, Blender's (its "Smooth by Angle" and former auto smooth), to be revisited on real use:

- **Boxes and placards stay square.** Their edges are 90°.
- **Flat surfaces of coarse scans stay clean.** On the campus, where walls, roofs and flat ground are a few large triangles, a larger angle spreads each wall corner over its neighbouring faces: at 180°, walls and roads turn into blotchy gradients, and 80° still smudges roofs. At 30° they read as flat planes.
- **Noisy and coarse curved surfaces look faceted.** Over the 67 local models with more than 1,000 triangles, the median share of edges sharper than 80° is 1.6 % (3.0 % above 60°, 0.95 % above 90°), but up to 27 % are sharper than 30° (59 % on `collection-34`). Trees on the campus and the stone of `collection-34` render close to flat at 30°, and smooth at 80°.
- **Creases cost vertices.** At 30°, `collection-34` gets 2.7 times its vertices, `scan-10m` and `collection-05` 28 % more; at 80°, 32 %, 0.3 % and 0.9 % more ([results](#results)).

80° was the alternative measured: smooth trees and stone, smudged roofs, fewer vertices. A crease test that ignores small noise spikes could get both, if a model shows it matters.

`tools/experiments/normals/render.py` renders a model in gray with Blender's Workbench, for this kind of comparison.

## In a chain

`normals` goes after `import` and before `weld` ([chaining.md](chaining.md#meta-commands)), and records no lossy round. After it, flat-normal primitives weld like smooth ones: the campus scan goes from 2.0 M vertices to 1.1 M.

## Results

Release build, inputs on a spinning disk and in the page cache, from `tools/experiments/normals/measure.sh` (`normals`, then `weld`, against `weld` alone). Times and heap (peak `RssAnon`, `tools/monitor.py`) are for `normals` on an imported chain, whose images are links: written as separate files, cave-b's 165 8k images take most of a minute to copy. The other times are from `measure.sh`.

| Model | Primitives | Vertices | After `normals` | Then `weld` | `weld` alone | Time | Heap |
| --- | --- | --- | --- | --- | --- | --- | --- |
| campus (flat, 634 primitives) | 634 smoothed | 2.00 M | 2.00 M | 1.13 M | 2.00 M | 0.8 s | 117 MiB |
| `collection-16` (flat) | 1 smoothed | 4.46 M | 4.46 M | 884,000 | 4.46 M | 1.4 s | 261 MiB |
| `cave-b` (no normals, no indices) | 166 added | 14.1 M | 14.1 M | 2.86 M | 2.65 M | 5.2 s | 263 MiB |
| `scan-10m` (no normals) | 30 added | 7.17 M | 9.20 M | 9.20 M | 7.17 M | 6.8 s | 262 MiB |
| `collection-24` (no normals) | 1 added | 3.00 M | 3.00 M | 516,000 | 512,000 | 1.2 s | |
| `collection-05` (no normals) | 1 added | 1.06 M | 1.34 M | 1.31 M | 1.03 M | 1.0 s | |
| `collection-34` (no normals, noisy) | 6 added | 143,000 | 388,000 | 388,000 | 143,000 | 0.2 s | |
| statue-b, normal map (meshopt, no normals) | 1 added | 247,000 | 285,000 | 276,000 | copied | 0.3 s | |
| `collection-01` (smooth) | 1 kept | 850,000 | | | | 0.03 s | |
| `scan-44m` (smooth and split) | 4 kept | 24.8 M | | | | 2.8 s | |

- **Flat scans weld once smooth.** The campus goes to 57 % of its vertices, collection-16 to 20 %. At 80°, the campus would reach 45 %, and 38 % without creases.
- **Creases add vertices on rough scans**: 28 % on `scan-10m` and `collection-05`, 15 % on statue-b, 2.7 times on the noisy `collection-34`; 0 to 1 % at 80° ([creases](#creases)).
- **Models with normals of their own cost a scan of their normals**, which stops at the first triangle whose normals differ: 0.03 s on a smooth scan. The 44M scan's 2.8 s is writing it out.
- **The campus, after `normals` and `weld`, compresses to 8.7 MB instead of 12.8 MB** (7.5 MB at 80°), and `simplify` then keeps 468,000 of its 672,000 faces instead of 649,000 (7.4 MB compressed, instead of 12.4 MB; 5.6 MB at 80°). collection-16 compresses to 7.7 MB instead of 35.7 MB, and to 0.61 MB after `simplify` (0.38 MB at 80°) (`tools/experiments/weld/measure.sh` on `normals`' outputs).
- **Checks:** `validate.mjs --against` finds no new issue on the campus, collection-16, statue-b, `collection-24` and `collection-34`, and `compare.mjs --geometry triangles --ignore NORMAL,TANGENT` finds each one equal to its input.

## Memory

Primitives that get normals are read whole into working arrays (`common::LargeArray`), with meshoptimizer's hash table in the same budget (`gltf::MeshoptMemory`). For each group of primitives in one space, the working set is about 20 bytes per corner (a position id per corner, the corners sorted by position, and one normal per corner) and 16 per vertex while positions are matched. Primitives that keep their normals are only scanned for flat normals, through cgltf, without copies. Rewritten primitives stay in memory until the model is written.
