# compress

How `meshtinker compress` quantizes and compresses geometry, and why. The code is `src/commands/compress/`, on the shared geometry editing of [gltf-io.md](gltf-io.md#editing).

## The preset

One preset, glTF-Transform's `meshopt({level: "high"})` with its `quantize` defaults, which the eCorpus compression guide picks over eCorpus's current `"medium"`: it stores normals on 8 bits instead of 10, for 29 % less geometry and no visible change on scans. There are no options.

| Attribute | Stored as | Bits |
| --- | --- | --- |
| `POSITION` | signed shorts on the scene's grid, not normalized (`KHR_mesh_quantization`) | 14 on the longest side |
| `NORMAL`, `TANGENT` | signed bytes, meshopt's `OCTAHEDRAL` filter | 8 |
| `TEXCOORD_n` in [0, 1] | normalized unsigned shorts | 12 |
| `COLOR_n` in [0, 1] | normalized unsigned bytes | 8 |
| `WEIGHTS_n` in [0, 1] | normalized unsigned bytes, rounding kept from changing each vertex's total | 8 |
| `JOINTS_n`, integer attributes, morph targets, values out of range | as they were stored | — |
| indices | unsigned shorts up to 65,535 vertices, unsigned ints beyond | — |

Values with fewer bits than their storage are rounded on their grid, then scaled to the storage's full range, as glTF-Transform stores them. Everything is then encoded with meshoptimizer (vertex codec version 0, which `EXT_meshopt_compression` requires; meshoptimizer 1.2 defaults to version 1): vertex attributes in `ATTRIBUTES` mode, triangle lists in `TRIANGLES` mode and other indices in `INDICES` mode. `EXT_meshopt_compression` and `KHR_mesh_quantization` are required, and the fallback buffer holds no data, as glTF-Transform writes them. Animations are left as they are: the preset's quaternion and exponential filters for keyframes aren't done.

## Order

A primitive whose accessors no other primitive uses is read as a triangle list (`gltf::readTriangles`), reordered for the vertex cache (`meshopt_optimizeVertexCacheStrip`), then for vertex fetch (`meshopt_optimizeVertexFetchRemap`), as glTF-Transform's `reorder({target: "size"})` does before compressing. Strips and fans become lists. Primitives that share accessors keep their order and mode, and each shared accessor is encoded once.

## Positions and node transforms

Positions are stored as `(p − center) / step`, rounded, on one grid for the whole scene: every mesh that can take a grid shares it, over their positions as stored, as gltfpack does by default and glTF-Transform does with `quantizationVolume: "scene"`. The grid spans their joint bounding box, with 2^13 − 1 steps on each side of the center along the longest axis, and the same step on every axis, so that normals need no correction. A shared grid makes the borders between tiles quantize to the same values on both sides, however the tiles are split into meshes. glTF-Transform's default quantizes each mesh on its own grid (`quantizationVolume: "mesh"`), which can open sub-step cracks between tiles. The cost is precision for small meshes beside large ones: on a 10 m model, a step is about 1.2 mm for every mesh.

The dequantization, translate(center) × scale(step), goes into each node that draws the mesh:

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
| Johannisfriedhof (local scan, 6 primitives) | 4.1 MB | 1.21 MB | 1.27 MB | | |
| DamagedHelmet | | 175 KB | 184 KB | | |
| Duck | | 27 KB | 29 KB | | |
| `scan-tiles` fixture | 267 KB | 44 KB | 39 KB | | |

"Out" and glTF-Transform's figure are the bytes of the compressed views, from glTF-Transform 4.5.1 with meshoptimizer 1.3.0, as pinned in `tools/`.
