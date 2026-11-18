# Placeholder

`meshtinker placeholder` builds a low-poly stand-in for a model, to display while the model is processed. It needs the right silhouette quickly, not fidelity. It follows objtools' `placeholder` command (branch `model_placeholder` of `../objtools`, `docs/wiki/Placeholders.md`), with fewer settings, and without texturing for now. The README gives its options and report.

Decimation scales poorly for this purpose: edge-collapse methods hold the whole mesh's connectivity in memory, and their quality falls at extreme ratios. A placeholder instead samples the model's surface onto a coarse grid, in one pass whose output size doesn't depend on the input.

## Input

The input is read as every command after `import` reads it ([normalized-profile.md](normalized-profile.md#how-commands-get-normalized-input)): any glTF or GLB, normalized in memory, so its default scene is the only scene and diagnostics name objects as `import` would. Faces are read in place through `gltf::GeometryReader`, so plain, quantized, sparse, meshopt and Draco data all work, and so do triangle strips, fans and non-indexed primitives. Plain float32 positions and 16- or 32-bit indices, the usual case for scans, are read directly. Anything else goes through cgltf's accessor functions.

Left out, with a counted warning:

- points and lines, which have no surface (`points-and-lines-left-out`);
- primitives whose data can't be decoded (`draco-decode-failed`, `meshopt-decode-failed`, `missing-vertex-data`);
- faces with an index out of range (`index-out-of-range`).

A scene that shows no triangles is rejected (`no-faces`), and so is one whose triangles span no extent (`no-extent`) or leave no surface at the chosen resolution (`no-surface`). A document without scenes is read through its root nodes, as the profile keeps everything then.

Only the rest pose is covered: animations and morph targets are ignored. Following them would take more than one surface.

## From triangles to a surface

**Voxelizing.** Cells are cubes, sized so that the model's longest axis spans `--resolution` of them (2 to 128, default 16), with one empty layer all around. Nearly every face of a dense mesh sits inside one cell, which comparing its corners' cells settles immediately. Other faces go through the separating-axis overlap test of Akenine-Möller (2001). Each cell accumulates the area of the surface crossing it and the area-weighted position of that surface, the point of each face closest to the cell's center (Ericson, *Real-Time Collision Detection*, 5.1.5). Accumulating per cell in one streamed pass follows Lindstrom's out-of-core simplification (2000), without its quadric error metrics.

Sums are fixed-point integers, 2^32 per cell face, so that they don't depend on their order: any thread count gives the same grid. A face too small to register still weighs one unit. A face's share of one cell is capped at √2 cell faces, the largest plane section of a cube. Only a face lying mostly outside a refitted grid can claim more, and it would overflow the sums.

**Dropping parts.** Groups of occupied cells connected by a face, edge or corner, with less than 0.5% of the largest group's area, are treated as scan noise and left out (`small-parts-left-out`). Removing a far-off floater can leave the model squeezed into a corner of the grid. So when the remaining parts span less than 90% of the longest axis, the model is voxelized again on a grid fitted to them, up to four passes in all. Parts are only told apart at the resolution of the pass that finds them, so the warning gives the largest count of any pass.

**Finding the outside.** A flood fill through shared cell faces marks everything reachable from outside the model. Whatever it can't reach counts as solid, so enclosed interiors produce no surface. An opening several cells wide lets the fill in, and inner walls are then kept. Closing such openings by morphological dilation isn't done: it costs exterior detail before it seals anything useful.

**Parts smaller than a cell.** A part fitting inside one cell has all its corners at that cell's mean position, so it collapses to a point and leaves no surface. That is harmless for noise, but a scene of objects far apart can lose some or all of them at a low resolution: they then need a higher `--resolution`.

**Extracting the surface.** Each face between a solid cell and an exterior cell becomes a quad, as in Surface Nets (Gibson 1998). Its vertices sit at the mean surface position of the occupied cells around them, rather than on the grid. The placeholder then follows the model instead of showing voxel steps, at the cost of beveling sharp edges by about one cell. Each quad is split along its shorter diagonal, and triangles under 10^-6 cell faces, slivers from collapsed vertices, are dropped. The surface can pass through the model by a fraction of a cell, which suits a placeholder: strict enclosure would look blockier and be up to a cell too large. A one-cell-thick sheet gets two coincident sides facing opposite ways.

**Normals.** Each vertex has the sum of its faces' area-weighted normals, normalized.

## Where the placeholder sits

A placeholder has to overlay what its source's default scene shows, whatever node transforms that scene uses. Baking them into world coordinates would be simple, but a placeholder's positions are float32, so a large translation would cost precision that the source's own positions never lost. So the grid is built in the frame of the scene's first mesh node, found depth first in file order, and that node's transforms are carried over with it.

- **The frame's nodes.** The placeholder copies the transform of each node from the scene's root down to that first mesh node, as the source wrote them, leaving out nodes without one. A single composed `matrix` wouldn't always be valid: under a parent's non-uniform scale, a rotated child is sheared, and glTF requires a node's `matrix` to decompose into translation, rotation and scale.
- **Other nodes.** When nodes have different transforms, each mesh is seen through the matrix from its node's coordinates to the frame, computed once per node or `EXT_mesh_gpu_instancing` instance. A node sharing the frame needs none, which leaves the hot loop as it would be with no transforms at all.
- **Flattening transforms.** A transform that flattens space can't be a frame, so the first mesh node that doesn't flatten it is used. A model flattened by all of them is built in world coordinates.
- **Skinned meshes.** The spec ignores a skinned mesh's node: a renderer places each vertex by its joints' world matrices times their inverse bind matrices. In a model whose rest pose is its bind pose, that product is the same for every joint, so the first joint's places the whole mesh, without reading joints and weights per vertex. No node carries that matrix, so a skinned node is never the frame. A model whose meshes are all skinned is built in world coordinates. objtools draws a skinned mesh as stored instead, which misplaces a skeleton under a transformed root (BrainStem's Z-up rotation).

The grid's cells are cubes in the frame's coordinates. Under a non-uniform scale they aren't cubes on screen, and the resolution follows the stored axes rather than the visible ones.

The grid's bounds are each primitive's box seen through each of its placements. Under a rotation that can be slightly larger than the geometry, which voxelizing again on a fitted grid makes up for: reading every vertex once per instance wouldn't scale. Each primitive's box comes from one pass over its positions, since accessor `min` and `max` may be missing or wrong.

## Output

`<stem>.gltf` and `<stem>.bin`, in the normalized profile: one scene, the frame's nodes, and one mesh named `placeholder` with one primitive. The `.bin` holds the indices first, then the vertices interleaved (position and normal, 24 bytes). Indices are 16-bit up to 65,535 vertices, since glTF reserves each type's maximum for primitive restart, and 32-bit beyond. The primitive has no material, so renderers draw glTF's default material. `asset.generator` names meshtinker and its version.

## Time, memory and threads

The faces are split between the machine's hardware threads, one per 100,000 faces at most, and each thread fills its own grid. The grids are merged at the end. The grids are the only structures that grow: 32 bytes per cell, per thread. At the default resolution of 16, a grid is at most about 190 kB. At the maximum of 128 it is at most about 70 MiB. The surface is extracted on one thread, and its size follows the resolution alone.

Measured on a 44-million-face scan with an 8 GB folder (4 cores):

| Resolution | Elapsed | Peak heap | Output faces |
| --- | --- | --- | --- |
| 16 | 6.6 s (file not cached) | 0.6 MB | 782 |
| 128 | 1.6 s (file cached) | 18 MB | 58,408 |

Reading the file sets the floor when it isn't cached, the normal case for a model larger than RAM.
