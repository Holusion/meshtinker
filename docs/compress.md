# compress

How `meshtinker compress` quantizes and compresses geometry, and why. The code is `src/commands/compress/`, on the shared geometry editing of [gltf-io.md](gltf-io.md#editing).

## The preset

One preset, glTF-Transform's `meshopt({level: "high"})` with its `quantize` defaults, which the eCorpus compression guide picks over eCorpus's current `"medium"`: it stores normals on 8 bits instead of 10, for 29 % less geometry and no visible change on scans. There are no options.

| Attribute | Stored as | Bits |
| --- | --- | --- |
| `POSITION` | normalized signed shorts holding the index on the scene's grid (`KHR_mesh_quantization`) | 14 on the longest side |
| `NORMAL`, `TANGENT` | signed bytes, meshopt's `OCTAHEDRAL` filter | 8 |
| `TEXCOORD_n` in [0, 1] | normalized unsigned shorts | 12 |
| `COLOR_n` in [0, 1] | normalized unsigned bytes | 8 |
| `WEIGHTS_n` in [0, 1] | normalized unsigned bytes, rounding kept from changing each vertex's total | 8 |
| `JOINTS_n`, integer attributes, morph targets, values out of range | as they were stored | — |
| indices | unsigned shorts up to 65,535 vertices, unsigned ints beyond | — |

Texture coordinates, colors and weights with fewer bits than their storage are rounded on their grid, then scaled to the storage's full range, as glTF-Transform stores them. Positions aren't scaled up (below). Everything is then encoded with meshoptimizer (vertex codec version 0, which `EXT_meshopt_compression` requires; meshoptimizer 1.2 defaults to version 1): vertex attributes in `ATTRIBUTES` mode, triangle lists in `TRIANGLES` mode and other indices in `INDICES` mode. `EXT_meshopt_compression` and `KHR_mesh_quantization` are required, and the fallback buffer holds no data, as glTF-Transform writes them. Animations are left as they are: the preset's quaternion and exponential filters for keyframes aren't done.

## Order

A primitive whose accessors no other primitive uses is read as a triangle list (`gltf::readTriangles`), reordered for the vertex cache (`meshopt_optimizeVertexCacheStrip`), then for vertex fetch (`meshopt_optimizeVertexFetchRemap`), as glTF-Transform's `reorder({target: "size"})` does before compressing. Strips and fans become lists. Primitives that share accessors keep their order and mode, and each shared accessor is encoded once.

## Positions and node transforms

Positions are stored as `(p − center) / step`, rounded, on one grid for the whole scene: every mesh that can take a grid shares it, over their positions as stored, as gltfpack does by default and glTF-Transform does with `quantizationVolume: "scene"`. The grid spans their joint bounding box, with 2^13 − 1 steps on each side of the center along the longest axis, and the same step on every axis, so that normals need no correction. A shared grid makes the borders between tiles quantize to the same values on both sides, however the tiles are split into meshes. glTF-Transform's default quantizes each mesh on its own grid (`quantizationVolume: "mesh"`), which can open sub-step cracks between tiles. The cost is precision for small meshes beside large ones: on a 10 m model, a step is about 1.2 mm for every mesh.

The grid index, within ±(2^13 − 1), is stored as it is in a normalized signed short, which a reader divides by 32767; the node transform multiplies it back by step × 32767, about twice the model's size. glTF-Transform also uses normalized shorts, but scales its 14-bit values up to the short's full range with a second rounding. The extra bits carry no precision, and they cost 4.5 to 13 % of the compressed geometry: storing our own index shifted up by two bits gives glTF-Transform's sizes within 0.01 %. Plain integers, unnormalized, would compress the same as ours, but would put the step itself in the node's scale, about 6e-5 for a model a unit across, which looks broken to anyone reading or editing the node tree.

16 bits instead of 14 would make the largest position error four times smaller (on a 78 m building, 0.6 mm instead of 2.4 mm), for 4.5 to 13 % more geometry. At `simplify`'s default error, 30 times larger than the 14-bit error, it changes nothing visible.

The dequantization, translate(center) × scale(step × 32767), goes into each node that draws the mesh:

- folded into the node's own transform (TRS or matrix), on the right, when nothing else depends on it;
- on a new child node that takes over the mesh, when the node has children or a camera, or an animation sets its transform. The child comes first among the node's children, so that traversal still reaches the mesh first. `meshes.addedNodes` counts them.

Positions stay float32, still compressed, when the node's transform can't carry a grid, or the grid would need more than a node:

- a skinned mesh: renderers ignore its node's transform;
- an instanced mesh (`EXT_mesh_gpu_instancing`): instance transforms apply before the node's;
- a mesh with morph targets: their deltas would need the grid's step too;
- a mesh with a primitive left as it is (Draco, meshopt), already quantized positions, or a POSITION accessor another mesh uses.

## What is kept

- **Draco primitives** are copied as they are, with a `draco-kept` warning: converting them to meshopt would be a choice for the caller ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)).
- **Already meshopt-compressed primitives** are copied, so compressing twice writes the same files.
- **Unreadable primitives** are copied with a `primitive-not-compressed` warning.
- A rewritten accessor keeps its source's `name`, `extras` and `extensions`.

## Results

| Model | Geometry in | Out | glTF-Transform "high" | Time | Heap |
| --- | --- | --- | --- | --- | --- |
| 44-million-face scan, 4 meshes | 1.32 GB | 207 MB (16 %) | | 20 s | 5.5 MB |
| The same, simplified first (954,000 faces) | 40 MB | 8.2 MB | | 0.6 s | 6 MB |
| collection-34 (local scan, 6 primitives) | 4.1 MB | 1.21 MB | 1.27 MB | | |
| DamagedHelmet | | 175 KB | 184 KB | | |
| Duck | | 27 KB | 29 KB | | |
| `scan-tiles` fixture | 267 KB | 44 KB | 39 KB | | |

"Out" and glTF-Transform's figure are the bytes of the compressed views, from glTF-Transform 4.5.1 with meshoptimizer 1.3.0, as pinned in `tools/`.
