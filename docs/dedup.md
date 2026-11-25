# dedup

How `meshtinker dedup` stores identical objects once, and why. The code is `src/commands/dedup/`, on the shared editing of [gltf-io.md](gltf-io.md#editing): it points references at the first of each set of equal objects, and `gltf::prune` removes the rest.

## What merges

Each kind is compared once the kinds it refers to are merged, in this order, so that two materials whose textures were copies of each other compare equal:

1. **Images** with the same bytes, whatever their name, URI or MIME type. A missing image file stays apart.
2. **Samplers**, then **textures**, then **materials**: equal JSON but for `name`. Textures compare their merged images and samplers, materials their merged textures.
3. **Accessors** with equal JSON but for `name`, `bufferView` and `byteOffset`, in buffer views with the same `target`, holding the same elements bit for bit. Strides don't matter: an accessor in an interleaved view merges with an equal one in a view of its own.
4. **Meshes**: equal JSON but for `name`, once their accessors and materials are merged. Nodes that drew copies of a mesh then draw one mesh.

This is glTF-Transform's `dedup()` with its defaults: it compares the same kinds, ignores names (its `keepUniqueNames` is off), and keeps the first of each set.

- **Names and extras.** The first object's `name` stays, and the names of the others go: `Stone_1` becomes `Stone_0`. Extras and extensions are compared like any other property, so two materials with different `extras` stay apart. Node names, which say what a part is, don't change.
- **Nothing a viewer draws changes.** Each node draws the same triangles, attributes, images and samplers. `test/dedup/dedup_test.cpp` checks this on the `duplicates` fixture and on fixtures with shared meshes, skins, instancing and morph targets.
- **Comparing bytes is cheap.** Images and accessors are grouped by their JSON first (size, type, count, bounds), and bytes are read only within a group of two or more. Equal JSON is rare on scans, whose accessors differ in their bounds: the survey below took 0.27 s on the 44-million-face scan, without reading its geometry.

## What's left as it is

- **Accessors in meshopt views**, whose bytes are compressed, **sparse accessors**, and **accessors without a view** (Draco's, or all zeros).
- **Byte and short matrices** (`MAT2`, `MAT3`), whose columns are padded: their padding bytes could differ.
- **Buffer views.** An accessor merged into another frees its bytes only when no other accessor still uses its buffer view. Exports that pack many accessors in one view, as animations often are, keep the view whole: on `collection-26`, 368 of 566 animation accessors merge and the `.bin` stays the same size.

## In a chain

`dedup` goes after `weld` and before `simplify` ([chaining.md](chaining.md#meta-commands)). It records no lossy round, and links the images and `.bin` it doesn't change.

- **After `weld`**, because `weld` copies a primitive that shares an accessor with another (`shared-vertices-kept`). Two meshes whose positions `dedup` merged, with different materials, would no longer be welded. Copies of a model are welded the same way, so they are still equal afterwards.
- **Before `simplify`**, which locks every position that two primitives share, so that tiles don't crack. Two copies of a mesh share every position, and are locked whole ([simplify.md](simplify.md#borders-between-primitives)); once merged, the one mesh is simplified.
- **Before `uv`, `ktx` and `avif`**, so that an image is baked and encoded once.

Commands that rewrite geometry write each primitive's own accessors. Accessors that `dedup` merged across meshes with different materials are then copies again; meshes it merged stay merged.

## Report

The objects of each kind before and after, and the bytes stored before and after: the buffer views' `byteLength`, and the images, each file once. Text by default, JSON with `--json`: `images`, `samplers`, `textures`, `materials`, `accessors` and `meshes` with `before` and `after`, `bytes.bufferViews` and `bytes.images` likewise, then `output.files`.

The hidden `--dry-run` reads the model and reports, and writes nothing, for surveys.

## Results

`--dry-run` over the corpus's 66 local models and 32 Khronos samples. Release build.

| Model | What merged | Bytes stored |
| --- | --- | --- |
| `collection-26` (local) | 368 of 566 accessors, all animation | unchanged: they share views |
| `MeshoptCubeTest` | 9 of 10 samplers, 27 of 109 accessors, 5 of 35 meshes | buffer views 15,920 → 11,528 bytes |
| `ChronographWatch` (WebP and KTX2) | 4 of 29 materials | unchanged |
| `AnimatedMorphCube` | 2 of 12 accessors | buffer views 4,284 → 3,708 bytes |
| `TextureSettingsTest`, `TextureCoordinateTest`, `MeshPrimitiveModes` | a texture, accessors that already shared bytes | at most 18 bytes |

- **No local model repeats an image, a material or a mesh**, and only the shark has duplicates at all. `dedup` matters for exports that repeat a texture or a mesh per material or per part, which the corpus lacks; the fixture stands in for them.
- **Checks:** `validate.mjs --against` finds no new issue on any output with duplicates. `compare.mjs` finds them equal, except for the names of merged materials (`ChronographWatch`). It can't read `MeshoptCubeTest`'s `KHR_meshopt_compression`, whose merged meshes reference accessors that already shared their data.
