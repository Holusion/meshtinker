# weld

How `meshtinker weld` merges vertices, and why. The code is `src/commands/weld/`, on the shared geometry editing of [gltf-io.md](gltf-io.md#editing).

## What merges

Within each triangle primitive, two vertices merge when they are equal in every attribute and every morph target's attributes, bit for bit. The one exception is −0, which counts as 0: the same value, rewritten as 0 in welded primitives. The comparison is meshoptimizer's `meshopt_generateVertexRemapMulti`, the hash-table approach that glTF-Transform's `weld()` settled on (its contributor notes credit meshoptimizer).

- **Nothing is moved or averaged.** Every corner of every triangle draws the values it drew before, in the same order (`test/weld/weld_test.cpp` checks this on the `soup` fixture). Two vertices at one position with different normals or texture coordinates stay apart: a UV seam stays a seam, a hard edge stays hard.
- **Every attribute counts**, including `JOINTS`, `WEIGHTS`, colors, custom attributes and morph targets. Vertices that differ only in what a morph target does to them stay apart.
- **Vertices no triangle uses are dropped** from welded primitives, as from any primitive a command rewrites.
- **Triangles are kept as they are**, including ones whose corners now share a vertex. Those already had zero area, so they draw nothing either way, and dropping them is for `simplify`.
- **There is no tolerance.** Merging vertices that are only close would move them: that is `simplify`'s job, or `compress`'s once it quantizes ([experiments/normals.md](experiments/normals.md#welding-within-a-tolerance) has the figures). Within `compress`'s grids, rounding merges under 1 % more vertices on real scans.

## What's left as it is

A primitive is copied as it is, with its original encoding, when:

- **nothing merges** and no vertex is unused;
- **it is Draco or meshopt compressed.** Their encoders already merged identical vertices, and rewriting the primitive would remove its compression ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)). There is no warning: nothing was missed;
- **it shares vertex data with another primitive** (`shared-vertices-kept`). Welding one would give it a copy of its own, and the shared data would stay for the others, so the file would grow;
- **it draws points or lines** (`points-and-lines-kept`), **or can't be read** (`primitive-not-welded`).

Welded primitives are written as every command that rewrites geometry writes them: float32 attributes, `JOINTS` in their source type, and strips and fans as lists. Quantized input comes out as float32, and `compress` quantizes it again, onto the same grid if nothing else changed.

## Normals, and how Voyager draws welded primitives

`weld` doesn't write, remove or compare normals in any special way: `NORMAL` is one attribute among the others. Primitives with flat normals, one per face, therefore merge almost nothing: on the 634-primitive campus scan, 2.0 M vertices become 1.997 M. Smoothing them is the job of `normals`, run before `weld`, by the rule that normals default to smooth (AGENTS.md, [normals.md](normals.md)).

A primitive without normals renders flat in a viewer that follows the spec, welded or not. eCorpus's viewer, Voyager, doesn't follow it: it computes normals with three.js's `computeVertexNormals`, which averages over the vertices a primitive shares. So in Voyager, welding a primitive without normals changes how it looks.
- A soup, flat before, becomes smooth.
- A mesh whose vertices were duplicated becomes smoother along its former duplicates.

That is the direction the smooth-normals rule asks for. `normals` writes them, smooth across UV seams, which makes the result the same in every viewer.

## In a chain

`weld` goes after `normals` and before `simplify`, which can only collapse edges between vertices that are shared ([chaining.md](chaining.md#meta-commands)). It records no lossy round.

## Results

Release build, inputs on a spinning disk and in the page cache, outputs on an SSD, from `tools/experiments/weld/measure.sh`.
- **Simplify columns:** `simplify` at its default error, without `weld` first and with it.
- **Compress columns:** the `.bin` that `compress` writes, alone and after `simplify`, each without `weld` and with it.

| Model | Vertices | After weld | Time | Faces | Simplify | Weld, simplify | Compress | Weld, compress | Simplify, compress | Weld, simplify, compress |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `Lascaux_seamed_8k` (no indices) | 14.1 M | 2.65 M | 1.2 s | 4.72 M | 4.71 M | 586,000 | 90.7 MB | 18.0 MB | 90.6 MB | 2.9 MB |
| `ISTI_dataset/RWT180` (dragon) | 9.31 M | 2.00 M | 1.0 s | 3.10 M | 3.10 M | 264,000 | 71.6 MB | 17.4 MB | 71.6 MB | 3.4 MB |
| `24-998-193-1-1-high` | 3.00 M | 512,000 | 0.4 s | 1.00 M | 1.00 M | 43,000 | 22.6 MB | 4.2 MB | 22.6 MB | 0.21 MB |
| `55-tasse-melissa-kintsugi` | 3.00 M | 545,000 | 0.3 s | 1.00 M | 1.00 M | 24,900 | 30.8 MB | 6.0 MB | 30.8 MB | 0.26 MB |
| campus (flat normals) | 2.00 M | 2.00 M | 0.2 s | 672,000 | 649,000 | 649,000 | 12.8 MB | 12.8 MB | 12.4 MB | 12.4 MB |
| `01-akhenaton-nefertiti` (already welded) | 850,000 | 850,000 | 0.1 s | 1.66 M | 13,600 | 13,600 | 7.9 MB | 7.9 MB | 97 KB | 97 KB |

- **On triangle soups, `simplify` does nothing without `weld`.** Every edge of a soup is a border, which meshoptimizer keeps. With `weld` first, the same error takes them to 2.5 to 12 % of their faces.
- **`compress` alone gains 4 to 5 times from it**, through fewer vertices and meshopt's index codec.
- **The campus scan doesn't merge**: its normals are per face. Besides, 18 of its 634 primitives share vertex data with another and are skipped (`shared-vertices-kept`). After `normals`, which gives each of them its own vertices, it welds to 1.1 M vertices ([normals.md](normals.md#results)).
- **The peak RSS** of `weld` is 0.1 to 0.8 GB, mostly the mapped input: every page of the buffers is read once.
- **Checks:** `validate.mjs --against` finds no new issue on any output, and `compare.mjs --geometry triangles` finds each one equal to its input.

## Memory

Each primitive is read alone, into working arrays (`common::LargeArray`), and meshoptimizer's hash table shares their budget (`gltf::MeshoptMemory`). Welded primitives stay in memory until the model is written, as in `simplify`.
