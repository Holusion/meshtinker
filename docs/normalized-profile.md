# Normalized profile

`meshtinker import` writes this profile, and every other command except `inspect` works on it and writes it. It starts from mesh2gltf's [Canonical glTF profile](https://github.com/Holusion/mesh2gltf/wiki/Canonical-glTF-profile), whose source is at `../mesh2gltf.wiki/Canonical-glTF-profile.md`. It keeps only the rules that make later processing simpler. Everything else in the input is passed through unchanged.

`import` implements it. Index renumbering, the buffer layout and image naming are described in [gltf-io.md](gltf-io.md).

## How commands get normalized input

Every command accepts any glTF or GLB file and normalizes it in memory as it reads it. It doesn't reject input outside the profile, and it doesn't call `import`.

- **A normalized document, read in place.** The reader selects the scene and builds the layout (`gltf::selectScene`, `gltf::normalizedLayout`), then parses the layout's JSON as a second `Document` over the same mappings. The input's own parse is then dropped. A command sees only this document, so input indices can't leak into its code. Nothing is copied: the one copy is the command's own output, written in the profile, which is the normalization's write step.
- **Indices mean the normalized model.** Diagnostics and reports number objects as `meshtinker import` would write them. That file always exists or can be made, and when the input was imported first its indices are the input's own.
- **`import` is a fixed point.** Importing an imported file writes identical files, so that a command's indices name the same objects whether or not `import` ran first.
- **Running `import` first is recommended.** Normalizing in memory renumbers but doesn't move data: on input outside the profile, buffer views are read in their source order rather than front to back. Presets run `import` first.
- **Experimental commands normalize their own way.** A variant built from another branch reads with its own version of the profile. Nothing checks a marker or a `generator` value, so commands built on different profile versions chain safely.

Detecting imported files by a marker (`asset.generator` or `asset.extras`) was rejected: it changes `asset`, which `import` passes through, and it says nothing true about a file another tool has edited since. Calling `import` from the wrapper or as a separate process was rejected too: the wrapper doesn't know each command's arguments and can't clean up after `exec`, and a spawned process would split one run's diagnostics and exit codes in two.

## Enforced

- **Files:** one `.gltf` JSON file, one external `<stem>.bin` as the only buffer holding data, and image files. No GLB and no data URIs.
- **References:** every `uri` is a relative path with no scheme, percent-encoded per path segment.
- **Buffer order:** the `.bin` holds buffer views in the order the primitives use them (meshes in order, then each primitive's indices, attributes, morph targets and Draco data), then every other view. A command walking primitives in order reads the mapped `.bin` front to back. Views start 4-byte aligned, which every accessor component type needs at most. Buffer views keep their order in the JSON: renumbering them would leave stale any index an unknown extension holds.
- **Sizes:** lengths and offsets in the `.bin` can exceed 4 GiB.
- **One scene:** the default scene, or scene 0 when `scene` is unset, as three.js's GLTFLoader does. The output has that scene only, with `scene` set to 0, and what it reaches. Everything else (nodes, meshes, skins, cameras, materials, textures, samplers, images, accessors, buffer views, animations) is dropped and the rest renumbered: objects of other scenes, and objects no scene reaches (orphans) alike. eCorpus ignores other scenes and nothing renders orphans, and this way the scene's figures describe the whole file.
  - "Reaches" follows every index the index graph knows ([gltf-io.md](gltf-io.md#selecting-and-renumbering)), so `KHR_materials_variants` materials stay with the primitives that list them.
  - An animation belongs to the scene of the nodes it targets.
  - A document without scenes keeps everything: there is no scene to choose, and dropping everything would leave an empty file. Its figures count every object.
- **Image files:** images from GLB chunks and data URIs become files, and the buffer views that only held them are dropped. File names follow [gltf-io.md](gltf-io.md#writing): a file image keeps its name, an embedded one is `<stem>_texture_<index>.<ext>`, and collisions are renamed the same way.

## Added to mesh2gltf's profile

- **Compressed geometry stays compressed.** `EXT_meshopt_compression` and `KHR_draco_mesh_compression` data is copied as is. A meshopt fallback buffer (no `uri`, `fallback: true`) stays a separate buffer without data, since it holds no bytes.
- **Quantized geometry stays quantized.** `KHR_mesh_quantization` attributes are copied as is. Their component types are not float32, so code that reads vertex data must not assume float32.
- **Alternative image sources:** a texture keeps its core `source` and its extension sources. Commands use the core `source` when there is one, with a warning, and an extension's source otherwise.
- **Images keep their format.** WebP, KTX2 and AVIF are kept, along with their extensions (`EXT_texture_webp`, `KHR_texture_basisu`, `EXT_texture_avif`). A command that decodes images (to resize or recompress them) reads PNG and JPEG with stb. It copies any other image unchanged and reports it with one counted warning, until it supports that format.
- **Unknown extensions and `extras` survive**, as do `extensionsUsed` and `extensionsRequired`.

## Commands that rewrite geometry

Reading meshopt and Draco is shared (`gltf::GeometryReader`); writing them back isn't. A command writes the primitives it rewrites uncompressed, with float32 attributes, and copies untouched primitives as they are. Compression is its own command, run last in presets, as eCorpus's `optimizeGlb` does. Otherwise "don't downgrade" would make a command that rewrites Draco primitives (simplify) re-encode Draco, and vendor the encoder too.

- Quality doesn't argue for compressed intermediates. Draco and meshopt are lossless after quantization, and quantizing again onto the same grid with the same bits changes nothing (Draco's grid is the primitive's bounding box, with one step for all axes). Every grid change adds at most half a step per axis, and the error grows about as √N over N roundtrips. A pipeline from compressed input therefore carries two roundings: the source encoder's and the final compression's. Keeping intermediates compressed would add one per geometry command, since each changes bounding boxes or creates off-grid positions.
- Draco → meshopt is no worse than Draco → Draco, as long as the bit depth is no coarser than the input's (normals are the risk: gltf-transform's Draco default is 10 bits, gltfpack's is 8; check gltf-transform's `meshopt()` defaults before relying on them). meshopt on float32 is lossless; with `KHR_mesh_quantization` the Draco grid can be reproduced exactly only when a mesh's primitives share it (scale and offset go in the node transform). No Draco encoder is needed: compression is meshopt only. eCorpus does the same (`optimizeGlb.ts`: `meshopt({ level: "medium" })`, then drops `KHR_draco_mesh_compression`). `compress` copies *untouched* Draco primitives as they are, under "don't downgrade" ([compress.md](compress.md#what-is-kept)); converting them would be an option of its own.
- `import` doesn't decode compressed geometry. It would make no reader simpler (every reader goes through `GeometryReader`), and it would blow up disk use (float32 Draco scans are several times larger), make texture-only pipelines pay for the decode, and put untouched primitives through an extra rounding at the final compression. The cost of deferring is on the write side: a command that rewrites some primitives must drop `KHR_draco_mesh_compression` from them, keep a compressed meshopt view while an untouched accessor still uses it (and the fallback buffer with it), and update `extensionsUsed`/`extensionsRequired`. That is `gltf::replaceGeometry` and `gltf::prune` ([gltf-io.md](gltf-io.md#editing)), tested on the Draco and shared-accessor fixtures. A fixture that mixes in one model a Draco primitive, two primitives sharing a meshopt view, a quantized primitive and a plain one is still missing. The remaining cost is decoding again a primitive that a previous command left untouched; `inspect` timings on the `large` Draco models will tell whether that matters.
- **Quantized attributes** follow the same rule. cgltf reads `KHR_mesh_quantization` data already. Rewritten attributes are written as float32, and quantization is left to the compression command.

## Not enforced

- **Origin handling:** no recentering, no `extras.mesh2gltf` frame. Positions keep their source values and nodes keep their transforms. With float32 positions in the input already, there is no precision to gain.
- **Image formats:** not restricted to PNG and JPEG (see above).
- **No geometry compression:** not enforced (see above).
- **Materials:** no conversion to a restricted metallic-roughness subset, no `extras.sourcePhong`, no `-untextured` or `-vertex-colors` copies. Those rules come from mesh2gltf's OBJ, PLY and VRML sources.
- **Vertex colors:** passed through, not reinterpreted.

## Not enforced yet

Left out because no command needs them yet. Each would apply only to uncompressed primitives, since enforcing it on compressed ones means decoding and re-encoding. Enforce one when a command becomes simpler for it.

- **Indexed triangle lists only** (indexing non-indexed primitives, converting strips and fans): `inspect` reads every mode already, and `simplify` writes the primitives it rewrites as indexed lists (`gltf::readTriangles`). Untouched primitives keep their mode.
- **Index component type** (`UNSIGNED_SHORT` up to 65,535 vertices, else `UNSIGNED_INT`): a size optimization, which belongs to an optimization command.
- **Interleaved or separate vertex views**: readers go through accessors, which handle both.
- **Primitives owning their buffer views**: this duplicates data the input shared, for no reader's benefit yet.
- **`POSITION` `min` and `max` always present and exact**: `inspect` computes exact bounds from the data, and nothing reads `min` and `max`.
