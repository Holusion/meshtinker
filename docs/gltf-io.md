# Reading and writing glTF

How `meshtinker::gltf` reads models and writes them back, and why. The code is `include/meshtinker/gltf/` and `src/gltf/`.

## Reading

`gltf::Document::read` accepts `.gltf` and `.glb` files.

- **Detection.** A file is glTF if it starts with the GLB magic, or if its first non-blank character (after an optional UTF-8 byte order mark) opens a JSON object. Anything else is rejected as `not-gltf`. cgltf would call an image or an OBJ "invalid JSON", which misleads. `asset.version` must be `2.x`, and `asset.minVersion`, if present, `2.0` (`invalid-gltf` otherwise).
- **Two parses.** cgltf parses the JSON into typed structures, which the code reads from. nlohmann parses the same bytes into `gltf::Json` (an `ordered_json`), which the writer passes through. Both index arrays the same way. Holding the JSON twice is acceptable: it is small next to the binary data.
- **Mapped data.** The model file and every buffer and image file are memory-mapped read-only (`common::MappedFile`). GLB chunks and buffer-view images are views into those mappings. Data URIs are decoded to the heap, because their text is already in the parsed JSON, so they never exceed it in size.
- **Buffers.** A buffer whose data is shorter than its `byteLength` is rejected (`buffer-too-short`). A buffer without `uri` is the GLB `BIN` chunk if it's buffer 0 of a GLB. Otherwise it must be a meshopt fallback buffer (`fallback: true` in `EXT_meshopt_compression` or `KHR_meshopt_compression`), which holds no data. Any other buffer without data is rejected (`buffer-missing`): the spec allows no other case, and the validator reports it as an error.
- **References.** Buffer and image `uri`s are percent-decoded (`%XX` only, so `#` and `?` stay part of the file name, as exporters write them unescaped), then resolved by `common::References`:
  - Without `--trusted`, the target must be a regular file inside the model's folder, after resolving symlinks. Absolute paths, drive letters and URI schemes are refused.
  - With `--trusted`, absolute paths and paths leading out are allowed, but URLs never are.
  - A refused reference and a missing file get the same message, so a model can't probe what exists outside its folder.
  - Unlike objtools, an absolute path is never replaced by a file of the same name beside the model.
- **Files.** `files()` lists the model file and then each referenced file once, with its size. The report's `input.files` comes from it.
- **Images.** Each image's bytes are found, and its format and size are read from the header (`gltf::readImageHeader`: PNG, JPEG, WebP, KTX2). Unrecognized bytes give an empty header; the caller decides what that means. A missing or refused image file is recorded as `ImageData::missing`, without bytes. The analysis rejects the model if a base color uses it, and warns otherwise.
- **Validation.** `cgltf_validate` runs after buffers are loaded. It checks index ranges, accessor bounds and buffer view ranges.

All rejections are `common::Failure`s with exit code 1 and a stable `code`. Opening errors follow `common::classifySystemError`: for example, a missing file rejects the input but a permission error is the machine's (exit 4).

## Reading geometry

`gltf::GeometryReader` makes any accessor readable through cgltf's accessor functions (`cgltf_accessor_read_float`, `cgltf_accessor_read_index`), which handle sparse accessors and normalized integers. It is the one place a decoder plugs in.
- `KHR_mesh_quantization` data is read in place; cgltf dequantizes normalized values, and the node transforms do the rest.
- `EXT_meshopt_compression` and `KHR_meshopt_compression` views are decoded with meshoptimizer, filters included, the first time an accessor needs them. The decoded bytes become the view's `cgltf_buffer_view::data`. Up to 4 MiB goes on the heap, larger views into workspace scratch files.
- `KHR_draco_mesh_compression` primitives are decoded with Draco's decoder when `GeometryReader::preparePrimitive` first sees them. Their attribute IDs are read from the JSON, since cgltf reads them as accessor indices. Each mapped attribute is converted to its accessor's component type, the faces are written as indices in the index accessor's type, and the accessors read that data until the reader is destroyed. Draco decodes a whole primitive on the heap before the copy: about 70 bytes per vertex, so a 25-million-vertex Draco primitive needs about 1.7 GB. This is the one accepted exception to "map, don't load": the environment's memory limits bound it, and a failed allocation exits with 3 (`out-of-memory`).

## Selecting and renumbering

- **The index graph.** `gltf::forEachReference` visits every index an object holds, by kind: the glTF schema plus the extensions listed in `graph.hpp`, including every material extension's `*Texture` objects. It serves both reachability and renumbering, so the two can't disagree.
- **Selection.** `gltf::selectScene` applies the profile's scene rule ([normalized-profile.md](normalized-profile.md)), and marks each object *kept* or dropped. Kept objects are what the scene reaches. Statistics count them, and `import` writes them.
- **Layout.** `gltf::normalizedLayout` builds the output JSON:
  - kept objects in source order, every known index renumbered, the scene as scene 0;
  - buffer views only images used dropped;
  - the `.bin` order of the views (primitive order);
  - the source → output index maps that `import` uses to renumber its report.
  An extension `isIndexAwareExtension` doesn't know may hold indices that go stale when anything is renumbered: `import` warns `extension-indices-not-renumbered`, once per such extension. `gltf::identityLayout` keeps everything as it is, for tests.

## Writing

`gltf::writeSeparate` writes a `Layout` as `<stem>.gltf`, one `<stem>.bin` and the image files into an existing directory. The files are created through `common::Outputs`, which never overwrites a file and removes what it created if the command fails.

- **JSON.** The layout's JSON passes through as nlohmann values. Besides the layout's renumbering, only `buffers`, the `buffer` and `byteOffset` of buffer views (and of their meshopt extensions), and image `uri` and `bufferView` change. Unknown extensions and extras therefore survive as values, with their key order kept. They are not byte-identical: numbers are written back in nlohmann's shortest round-trip form (`1e2` becomes `100.0`), and `\u` escapes become UTF-8. The output JSON is compact.
- **Buffers.** Every buffer with data is merged into `<stem>.bin`, which becomes buffer 0. The bin is left out when there is no data. Buffers without data (meshopt fallbacks) follow, unchanged. The names, extras and extensions of merged buffers are not kept: exporters put only default names there (the model's name, `"Duck"`), and no corpus model has buffer extras or extensions.
- **Buffer views.** Each view's bytes are copied into the bin in the layout's order, each starting 4-byte aligned. That is enough for every accessor component type. The compressed bytes of a meshopt view are copied the same way, and its extension's `buffer` and `byteOffset` point to them. A view into a fallback buffer keeps its offsets.
- **Images.** Every image becomes a file, and its `uri` is the file name with every byte except `A–Z a–z 0–9 - . _ ~` percent-encoded. Names come from `gltf::imageFileNames`:
  - An image from a file keeps that file's name, without its folders.
  - An image from a buffer view or a data URI is named `<stem>_texture_<index>.<ext>`, with the extension of its format.
  - A name that collides with an earlier one or with the output's own files, ignoring case, is renamed the same way.
  - Images that share a source file share one output file.
- **Large files.** Offsets and sizes are 64-bit. Bytes are copied from the mappings in large writes, so the heap never holds buffer data.

