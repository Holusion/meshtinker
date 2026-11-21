# simplify

How `meshtinker simplify` reduces a model's triangles, and why. The code is `src/commands/simplify/`, on the shared geometry editing of [gltf-io.md](gltf-io.md#editing).

## What it does

The whole scene is read at once. Every triangle primitive is read as a triangle list (`gltf::readTriangles`: strips and fans become lists with their winding, degenerate triangles from strips are dropped), then simplified on its own with `meshopt_simplifyWithAttributes`, from meshoptimizer 1.2:

- **Error, no target count.** The target index count is 0, so meshoptimizer collapses edges until its error reaches the limit, as far as the limit allows and no further. A primitive that loses nothing is copied as it is. This is the "best effort" mode: no fallback that forces a count, unlike eCorpus's presets, which add a ratio (`optimizeGlb.ts`).
- **Measured on the scene, in world space.** The limit is `--error` × the longest side of the scene's bounding box, through every node that draws each mesh (an instanced node contributes its mesh's bounding box for each instance). It's converted to each mesh's own units by the largest axis scale of the nodes and instances that draw it, the strictest limit, and passed as an absolute distance (`meshopt_SimplifyErrorAbsolute`). meshoptimizer's default would measure each primitive against its own size, so the tiles of one scan would each get a different limit, and glTF-Transform's `simplify`, which eCorpus runs, does so. Skinned meshes are placed by their node, as `inspect` places them.
- **Emptied primitives are removed.** A primitive that would lose every triangle, smaller than the error, is removed at once (`primitive-removed`, counted in `primitives.removed`), and the shared prune takes the mesh, material, textures and images only it used ([gltf-io.md](gltf-io.md#editing)). The node that drew it stays, without its mesh, since nodes can carry other things (lights, extras, children).
- **Positions only.** Normals, texture coordinates and colors don't weigh in the error, as with glTF-Transform's `simplify`. meshoptimizer keeps attribute seams on its own: vertices at the same position with different attributes collapse together or not at all.
- **Small parts go.** Disconnected parts smaller than the error, such as floating scan noise, are removed (`meshopt_SimplifyPrune`): no collapse could keep them at that size, and they make no sense at that level of detail. A primitive made only of such parts loses every triangle, and is removed. The `noise` fixture has a 0.05 and a 0.001 cube beside a 2-unit one: the default error removes the 0.001 cube only. On the 44-million-face scan it removes 16,600 more faces at the default error (936,966 left instead of 953,560).
- **No permissive mode.** Seams are never crossed (`meshopt_SimplifyPermissive` is off).

The output keeps the vertices the simplified triangles use, in the order they first appear (`meshopt_optimizeVertexFetchRemap`), with every attribute and morph target remapped. Simplification only removes vertices: each output vertex is an input vertex, with all its values. Positions and other attributes are written as float32, JOINTS as unsigned bytes or shorts as in the source, and indices as unsigned shorts up to 65,535 vertices. Draco, meshopt and quantized primitives that are simplified are written plain, and the extensions go once nothing uses them ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)). Compression is left to its own command.

## Borders between primitives

Each primitive is simplified alone, so a border that two primitives share would be collapsed differently on each side and open a crack. Tiled scans have such borders, in two layouts: one mesh in several primitives (up to 634 in the local scans), or one mesh per tile, as the 44-million-face scan writes its 4 UDIM tiles, on nodes without transforms, sharing 10,000 to 77,000 border positions pairwise. Vertices whose exact position the triangles of any other primitive of the scene also use are locked (`vertex_lock`): they stay, and so do the border edges between them. On the 44-million-face scan, every shared position is still shared after simplification.

- Positions compare bit for bit as stored, with −0 as 0, without node transforms: tiles share their borders' values when their nodes have the same transform, which is how exports write them. Only vertices the primitive's triangles use count, so primitives that share one vertex accessor lock only what both draw.
- Two separate meshes that are copies of each other, each with its own copy of the data, share every position, and are locked whole. No local model does this. Locking only positions on open borders would avoid it, for a pass over the edges.
- Finding shared positions sorts every vertex position of the scene: 3 s for the 44-million-face scan's 24.8 million vertices, and 2 s to look each vertex up, against 55 s of simplification.
- Locking costs simplification along the borders. The 44-million-face scan keeps 954,000 faces (before small parts are removed), where it kept 491,000 with its borders between meshes left free to crack.

## Triangle soups

A primitive whose triangles share no vertices, three per triangle, gives meshoptimizer no edges to collapse: every edge is a border. Some local scans are written this way: the 634-primitive scan (2.0 million vertices for 672,000 faces) loses 3 % of its faces at the default error, and `Lascaux_seamed_8k` (4.7 million faces in 166 non-indexed primitives) 0.03 %. Welding identical vertices first, which eCorpus does with glTF-Transform's `weld`, would be its own command.

## How far the error is from the distance

meshoptimizer's error is a quadric estimate: the distance from the moved vertices to the planes of the triangles they collapsed from. It isn't a bound on the distance from the input surface to the output. On the `scan-tiles` fixture, the farthest input vertex is 1.5 times the error away from the output at 0.001, and 2.1 times at 0.01 (`test/simplify/simplify_test.cpp`). The report's `error.achieved` is meshoptimizer's figure, relative to the scene's size.

## Memory

The scene's primitives are read at once, into `common::LargeArray`s, to find their shared borders and the scene's size. Simplified primitives stay in memory until the model is written. Both go on the heap only while all working arrays hold 256 MiB, and in scratch blocks beyond, and meshoptimizer's own allocations share that budget (`gltf::MeshoptMemory`). On the 44-million-face scan the heap peaks at 256 MiB, in 55 s on an SSD (50 s with no budget, 59 s with a 4 MiB one, [experiments/heap-budget-results.md](experiments/heap-budget-results.md)). The workspace holds the whole scene decoded to float32, and the simplified primitives until the write.
