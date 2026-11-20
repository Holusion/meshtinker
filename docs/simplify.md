# simplify

How `meshtinker simplify` reduces a model's triangles, and why. The code is `src/commands/simplify/`, on the shared geometry editing of [gltf-io.md](gltf-io.md#editing).

## What it does

For each mesh, every triangle primitive is read as a triangle list (`gltf::readTriangles`: strips and fans become lists with their winding, degenerate triangles from strips are dropped), then simplified on its own with `meshopt_simplifyWithAttributes`, from meshoptimizer 1.2:

- **Error, no target count.** The target index count is 0, so meshoptimizer collapses edges until its error reaches the limit, as far as the limit allows and no further. A primitive that loses nothing, or would lose everything, is copied as it is. This is the "best effort" mode: no fallback that forces a count, unlike eCorpus's presets, which add a ratio (`optimizeGlb.ts`).
- **Measured on the mesh.** The limit is `--error` × the longest side of the bounding box of all the mesh's primitives, passed as an absolute distance (`meshopt_SimplifyErrorAbsolute`). meshoptimizer's default would measure each primitive against its own size, so the tiles of one scan would each get a different limit. glTF-Transform's `simplify`, which eCorpus runs, measures per primitive. Meshes in different nodes are measured separately, in their own units: node transforms aren't applied.
- **Positions only.** Normals, texture coordinates and colors don't weigh in the error, as with glTF-Transform's `simplify`. meshoptimizer keeps attribute seams on its own: vertices at the same position with different attributes collapse together or not at all.
- **No pruning, no permissive mode.** Small disconnected parts stay (`meshopt_SimplifyPrune` is off), and seams are never crossed (`meshopt_SimplifyPermissive` is off).

The output keeps the vertices the simplified triangles use, in the order they first appear (`meshopt_optimizeVertexFetchRemap`), with every attribute and morph target remapped. Simplification only removes vertices: each output vertex is an input vertex, with all its values. Positions and other attributes are written as float32, JOINTS as unsigned bytes or shorts as in the source, and indices as unsigned shorts up to 65,535 vertices. Draco, meshopt and quantized primitives that are simplified are written plain, and the extensions go once nothing uses them ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)). Compression is left to its own command.

## Borders between primitives

Each primitive is simplified alone, so a border that two primitives share would be collapsed differently on each side and open a crack. Tiled scans have such borders: several local scans are one mesh in up to 634 primitives, each tile with its own vertices along the seams. Vertices whose exact position the triangles of another primitive of the same mesh also use are locked (`vertex_lock`): they stay, and so do the border edges between them.

- Positions compare bit for bit, with −0 as 0. Only vertices the primitive's triangles use count, so primitives that share one vertex accessor lock only what both draw.
- Borders between different meshes aren't locked: their vertices are in different spaces once node transforms apply.
- Locking costs simplification along the borders. On a tiled scan with many small tiles (634 primitives, 672,000 faces), only 3 % of the faces go at the default error, against 92 % on the same kind of scan in 16 tiles.

## How far the error is from the distance

meshoptimizer's error is a quadric estimate: the distance from the moved vertices to the planes of the triangles they collapsed from. It isn't a bound on the distance from the input surface to the output. On the `scan-tiles` fixture, the farthest input vertex is 1.5 times the error away from the output at 0.001, and 2.1 times at 0.01 (`test/simplify/simplify_test.cpp`). The report's `error.achieved` is meshoptimizer's figure, relative to the mesh's size.

## Memory

A mesh's primitives are read at once, into `common::LargeArray`s, to find their shared borders. Simplified primitives stay in memory until the model is written. Both go on the heap only while all working arrays hold 4 MiB, and in scratch blocks beyond, and meshoptimizer's own allocations share that budget (`gltf::MeshoptMemory`). On the 44-million-face scan, the heap peaks at 4.5 MB. The workspace holds the mesh being simplified, decoded to float32, and the simplified primitives until the write.
