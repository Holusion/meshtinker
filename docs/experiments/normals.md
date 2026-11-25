# Experiment: how real models store normals, and what welding them gains

This is the groundwork for `weld` and `normals` ([weld.md](../weld.md), [normals.md](../normals.md)). The rule it applies comes from AGENTS.md: **normals default to smooth**. Flat normals, and missing ones, are presumed to be a mistake made upstream, unless the data shows the hard edges are meant.

The script is `tools/experiments/normals/survey.mjs`. It ran on every model of `test/corpus.json` (fixtures, Khronos samples, local models), plus the large local models of [heap-budget-results.md](heap-budget-results.md). The figures per model are in [normals-survey.csv](normals-survey.csv): 136 files, 70 of them local. Its last three columns split the edges above 60°, for the local models with more than 1,000 triangles.

## What the survey measures

Per triangle primitive:

- **Class.**
  - `no-normals`: no `NORMAL` (`-unindexed` if there are no indices either).
  - `flat`: every triangle's three normals are within 1° of its face normal. `mostly-flat` means more than half of them are.
  - `smooth`: no position carries two different normals.
  - `split`: some positions do.
- **Welds.** These are vertex counts after merging the vertices that are equal in:
  - every attribute, bit for bit (`exact`);
  - every attribute but `NORMAL`;
  - `POSITION` alone;
  - every attribute rounded on `compress`'s grids (`quantized`): 14-bit positions on the scene's grid, 8-bit normals, and 12-bit texture coordinates over their range.
- **Dihedral angles.** For each edge two triangles share (by position), the angle between their faces, in buckets: ≤1°, ≤10°, ≤30°, ≤60°, more. Edges above 60° are split again: ≤80°, ≤90°, more (on the local models with more than 1,000 triangles).
- **Split positions.** For each position, the largest angle between its vertices' normals, in the same buckets.

## Where normals come from

Local models, by the class of most of their primitives:

| Class | Files | Triangles | After exact weld | After weld without normals | Position alone |
| --- | --- | --- | --- | --- | --- |
| no normals, indexed | 39 | 25.0 M | 86.9 % | 86.9 % | 66.7 % |
| no normals, no indices (`cave-b`) | 1 | 4.7 M | 18.7 % | 18.7 % | 18.6 % |
| flat (the campus scan, 3 boxes and planes of ≤12 triangles) | 4 | 0.67 M | 100 % | 38.3 % | 21.6 % |
| mostly flat (`collection-16`) | 1 | 1.5 M | 100 % | 19.4 % | 16.8 % |
| smooth | 13 | 92.6 M | 90.5 % | 90.5 % | 89.6 % |
| split | 12 | 6.1 M | 38.4 % | 38.1 % | 25.8 % |

Percentages are of the input's vertices.

- **Most local scans have no normals: 40 of 70 files.** `obj2gltf` writes none when the OBJ has no `vn` lines, and the collection's OBJ sources mostly have none. Some GLB sources have none either.
- **The Khronos samples and fixtures are the opposite.** Their flat primitives (17 sample models, 20 fixtures) are cubes and planes, and their split normals are authored hard edges: DamagedHelmet, Duck, ChronographWatch.

## The cases

1. **No `NORMAL` at all.** The glTF spec says a viewer must render it flat, and three.js does (`GLTFLoader` sets `flatShading`). eCorpus's viewer overrides that: Voyager's `ModelReader` calls three.js's `computeVertexNormals` and turns flat shading off, with a console warning.
   - **On indexed meshes, that's smooth, but only within shared vertices.** Normals are summed per vertex index, so they split wherever the file duplicates a vertex at a UV seam. Those duplicates are 0.4 to 52 % of the vertices of the indexed no-normal scans (median 33 %). So Voyager shows a shading crease along every UV chart border today.
   - **On soups it's flat.** `cave-b` has no indices. `collection-24` is indexed but has three vertices per triangle: welding takes it to 17 %.
2. **Stored normals that are all face normals.** On the campus scan (634 primitives, 672,000 triangles), each triangle's three normals are bit-identical and match its face (worst dot product 0.99996). `collection-16` is the same on 1.5 M triangles. Both are scans:
   - Their dihedral angles spread continuously: on the campus, 23 % of edges are between 1° and 30°, and 52 % on collection-16.
   - Nothing in the data points to deliberate facets: it's case 1 after a tool computed normals per face.
   - Since the normals are stored, every viewer renders them flat, Voyager included.
3. **Flat boxes and planes.** These are three local placards of ≤12 triangles (`collection-45`, `collection-46`, `collection-49`) and the Khronos cubes. Their dihedral angles are 0° or 90° and nothing in between. The hard edges are meant. Smoothing with a crease angle below 90° keeps them, while smoothing a scan everywhere.
4. **Authored split normals.** In DamagedHelmet, ChronographWatch and Duck, positions carry several normals 30° to 180° apart, along edges whose dihedral angles are as large. That is a deliberate hard edge, and nothing is in doubt.
5. **Smooth, with splits of under 1° at UV seams.** In `collection-33`, 92 % of positions are like this. It renders smooth. The vertices differ in UV anyway, so no weld would merge them.
6. **Smooth normals stored on unwelded vertices.** Examples are `collection-55` (an exact weld leaves 18 %), `sculpture-3m` (22 %), `specimen` (57 %) and the cave-a tiles (65 %). The exact weld recovers them with nothing to decide.
7. **Normal maps on primitives without normals:** 15 primitives in no-normal local models, among them `statue-a`'s test files. Voyager computes vertex normals and tangents for them as in case 1.
8. **Dense meshes look flat by angle alone.** On the 44-million-face scans, 31 % of triangles are within 1° of their face normal, simply because neighbouring faces differ by less than that: 54 % of their dihedral angles are ≤1°. A test for case 2 must check that a triangle's three normals are identical, not only close to the face's.

## What a smooth normal should be

The rule only matters where the normals are missing or all flat: cases 1 and 2. Authored normals (cases 4 to 6) are kept as they are.

What "smooth" means there:

- **Sum over positions, not vertex indices.** Otherwise UV seams leave creases, as Voyager's normals do today.
- **Sum across the primitives that share a border**, as `simplify` already finds them (shared positions, same node transform). Otherwise the 634 tiles of the campus scan would show their borders.
- **Keep edges sharper than a crease angle hard.** Boxes have 90° edges. On scans with more than 1,000 triangles, 0 to 9.4 % of edges are above 60°, mostly noise, and 22 to 34 % on four of them (`33-collection-33-collection-33`, the campus, `collection-34`, `collection-08`). Any crease angle up to 90° keeps boxes square; a higher one leaves more of a scan smooth.
  - Above 80°: a median of 1.6 % of edges, and 0 to 4.8 % on all but those four (11.5 to 20 %).
  - Above 90°: a median of 0.95 %.
  - Blender's auto-smooth default of 30° would crease up to 27 % of a scan's edges (59 % on `collection-34`).
  - three.js's `toCreasedNormals` default of 60° would crease up to 9.4 % on all but those four.
  - `normals` uses Blender's 30°, to be revisited on real use; [normals.md](../normals.md#creases) compares it with 80° on renders.
- **Weighting:** by corner angle or by face area. three.js, and therefore Voyager, weights by area. `normals` weights by angle, as Blender does: it doesn't depend on how a surface is cut into triangles.

## What it means for `weld`

- **`weld` stays exact.** It merges vertices equal in every attribute, and doesn't touch normals. On the files where that gains anything, the gains are large: 81 % on cave-b, 83 % on `collection-24`, 82 % on the collection-55 cup, 78 % on sculpture-3m.
- **In Voyager, welding still changes how a no-normal primitive looks.** Its computed normals follow shared vertices, so a welded soup goes from flat to smooth. That is the direction the rule asks for. Spec-compliant viewers still render it flat.
- **Normals are better written by a command of their own than computed by the viewer.** `normals` applies to cases 1 and 2 only, by default, with `--keep-flat` to keep flat normals. It goes before `weld` in a chain: once it has written smooth normals, the campus scan's vertices become identical, and an exact weld takes it from 2.0 M vertices to 0.9 M, a little above the 0.77 M of the weld without normals, for its creases. It goes before `simplify` too, so that `simplify` sees welded geometry.

## Welding within a tolerance

`compress` rounds every attribute on its grids, so two vertices that land on the same values are identical in its output. Merging them there loses nothing that quantizing hasn't already lost.

On the measured scans, it barely gains anything:
- **Scans with smooth or no normals:** rounding merges at most 0.83 % more vertices than the exact weld (`collection-36`), and under 0.2 % on all but three. Near-duplicate positions are rare.
- **Flat-normal files:** the 8-bit normals of nearly coplanar faces merge, so the campus goes to 72 %. The normals command reaches 38 %, and smooths it as well.

A tolerance before `simplify` would only matter for cracks, positions apart by float noise along a border. The figure above bounds how many such vertices there are. If a model ever needs it, it would be an option of `weld`, in units of the grid that `compress` will use, since `weld` can't guess that step. Merging identical quantized vertices inside `compress`, which knows its grids, needs no option. It is cheap once `weld`'s remap exists, so it goes there, measured again on `simplify`'s outputs ([chaining.md](../chaining.md#meta-commands) says why `compress` isn't split in two for it).
