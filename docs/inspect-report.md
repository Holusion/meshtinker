# Inspection report

`meshtinker inspect --json` prints this report on stdout, and `meshtinker import --json` prints the same report for its output. Its first consumer is eCorpus, where it replaces the gltf-transform inspection (`source/server/utils/gltf/inspect.ts` and `warnings.ts`). eCorpus's TypeScript types will be generated from this document, so it is the reference for the shape. Text output is rendered from the same data.

`inspect` and `import` accept and reject the same inputs, because they run the same pipeline (`analysis::inspect`). `import`'s report describes the files it wrote, with their indices: inspecting `import`'s output gives the same report, which ctest checks for every fixture. Indices in a report are those of the inspected file.

`inspect --no-data` prints a lighter report, which reads the model file alone: see [Without data](#without-data---no-data).

Rejected inputs (exit code 1, with `error.code`): files that aren't glTF 2.0 (`not-gltf`, `invalid-json`, `invalid-gltf`), references that are missing or lead outside the model's folder without `--trusted` (`reference-refused`), buffers shorter than their `byteLength` (`buffer-too-short`) or without data (`buffer-missing`), data URIs that aren't base64 (`invalid-data-uri`), and a missing image file that the scene reaches (`missing-image`): a normal or occlusion map changes what the model looks like as much as a base color does, and an alternative source (WebP, KTX2, AVIF) is what a viewer supporting it loads. Everything else is reported, with diagnostics where figures are incomplete.

## Example

```jsonc
{
  "format": 1,                                // incremented when a field changes meaning or is removed
  "tool": { "name": "meshtinker-inspect", "version": "0.1.0" },
  "data": true,                               // false with --no-data: see below

  "input": {
    "byteSize": 52428800,                     // sum of the files below
    "files": [
      { "role": "gltf", "name": "model.gltf", "byteSize": 20480 },
      { "role": "buffer", "name": "model.bin", "byteSize": 40000000 },
      { "role": "image", "name": "diffuse.jpg", "byteSize": 12408320 }
      // roles: "gltf" | "glb" | "buffer" | "image"
    ]
  },
  "requires": [["model.bin"], ["diffuse.jpg"]], // files the model is rejected without

  "extensions": {
    "used": ["EXT_meshopt_compression", "KHR_texture_transform"],
    "required": ["EXT_meshopt_compression"]
  },

  "scene": {
    "index": 0,
    "name": "Scene",                          // null when unnamed
    "firstNamedRootNode": "Cube",             // null when no root node has a name
    "bounds": {                               // world space; null when it can't be computed exactly
      "min": [-1.0, -1.0, -1.0],
      "max": [1.0, 1.0, 1.0]
    }
  },

  "geometry": {                               // each mesh counted once, however many nodes use it
    "meshes": 1,
    "primitives": 1,
    "vertices": 24,
    "faces": 12,
    "primitivesByEncoding": { "plain": 0, "meshopt": 1, "draco": 0, "quantized": 0 }
  },

  "images": [
    {
      "index": 0,
      "name": "diffuse",                      // null when unnamed
      "file": "diffuse.jpg",                  // null when embedded (GLB chunk or data URI)
      "mimeType": "image/jpeg",
      "width": 4096,
      "height": 4096,
      "byteSize": 12408320,
      "uses": [
        { "material": 0, "slot": "baseColorTexture", "texture": 0 }
      ],
      "occupancy": 0.4712                     // null when not computed
    }
  ],
  "texels": { "total": 16777216, "occupancy": 0.4712 },

  "diagnostics": [
    {
      "code": "texture-alternative-source",
      "severity": "warning",                  // "warning" | "error"
      "count": 1,
      "message": "A texture has both a core source and an extension alternative (WebP, KTX2, AVIF): the core source is the one measured",
      "examples": ["texture 0"]
    }
  ]
}
```

When the command fails and `--json` is set, stdout still carries one JSON object. It has `format`, `tool`, the diagnostics gathered so far, and an `error` instead of the statistics:

```jsonc
{
  "format": 1,
  "tool": { "name": "meshtinker-inspect", "version": "0.1.0" },
  "error": { "code": "invalid-glb", "exitCode": 1, "message": "GLB chunk length exceeds file size" },
  "diagnostics": []
}
```

## Fields

The cost column says what a field needs. **JSON** fields come from the model's JSON alone, and are the ones a [report without data](#without-data---no-data) has. **Document** fields also need file sizes and image headers. **Data** fields read buffer contents or rasterize triangles. JSON and Document fields are computed in a first stage (`analysis::analyzeDocument`), Data fields in a second (`analysis::analyzeData`), which `--no-data` skips.

| Field | Meaning | Cost |
| --- | --- | --- |
| `data` | `true`, or `false` in a [report without data](#without-data---no-data). | JSON |
| `input.byteSize`, `input.files` | Every file the model consists of. A GLB is one file. | Document |
| `requires` | The files the model refers to (`buffers[].uri`, and `images[].uri` of the images below), as paths relative to its folder: decoded, with forward slashes, without `.` and `..` steps, each once. The model is rejected without any of them. Buffers come first, then images, in index order. Each entry is the list of paths the file is looked for at, in order, as in mesh2gltf's `--inspect`: meshtinker looks at one. Data URIs and GLB chunks aren't files, so a GLB has `[]`. A reference the text refuses isn't listed. | JSON |
| `extensions.used`, `.required` | As declared in the input. | JSON |
| `scene` | The default scene, or scene 0 when `scene` is unset, as three.js's GLTFLoader does. Only this scene is inspected; see [the profile](normalized-profile.md). | JSON |
| `scene.firstNamedRootNode` | The first root node of the scene with a non-empty name. eCorpus names models after it. | JSON |
| `scene.bounds` | Exact world-space bounds of every vertex the scene's primitives reference, all modes included, through each node's world matrix. Every `EXT_mesh_gpu_instancing` instance counts, at the node's world matrix times the instance's. Morph targets and skins are ignored, as gltf-transform's `getBounds` does. meshopt, Draco and `KHR_mesh_quantization` data are decoded. Never approximated from accessor `min`/`max`: `null` when the scene has no geometry, or when a primitive's data can't be read (with a diagnostic below). | Data |
| `geometry` | Geometry the scene uses. Each mesh counts once, however many nodes reference it, because a shared mesh is stored and uploaded once and no later command duplicates it. This matches objtools and gltf-transform. | JSON |
| `geometry.meshes`, `.primitives` | Meshes the scene reaches, and their primitives (every mesh when the document has no scene). | JSON |
| `geometry.vertices` | Vertices uploaded to the GPU: each `POSITION` accessor counted once, as gltf-transform's `VertexCountMethod.UPLOAD` does. | JSON |
| `geometry.faces` | Triangles. Triangle lists count `n / 3`, strips and fans `n − 2`, points and lines none. | JSON |
| `geometry.primitivesByEncoding` | Primitives by how their vertex data is stored; the values add up to `primitives`. `quantized` means `KHR_mesh_quantization` without compression. | JSON |
| `images` | Every image `import` keeps: those the scene reaches. Images of other scenes and images nothing uses are left out. | JSON |
| `images[].file` | The file the image is read from, as its `uri` names it (decoded). `null` for an image in a GLB chunk or a data URI. In `import`'s report, every image is a file. | JSON |
| `images[].mimeType`, `.width`, `.height` | From file headers, without decoding pixels. PNG, JPEG, WebP and KTX2. `null` for anything else, with an `image-unrecognized` diagnostic. | Document |
| `images[].byteSize` | The encoded image's size. 0 for a missing file. | Document |
| `images[].uses` | Every slot of a material the scene's primitives use that reaches the image. Materials only `KHR_materials_variants` use don't count: nothing renders them by default. A slot is the property path from the material, such as `baseColorTexture` or `KHR_materials_clearcoat.clearcoatTexture`. A texture's core `source` is used even when an extension gives an alternative (WebP, KTX2), with a `texture-alternative-source` diagnostic. An extension's source is used only when there is no core `source`. | JSON |
| `images[].occupancy`, `texels.occupancy` | Fraction of texels sampled when rendering, as defined by objtools' [texture occupancy](../../objtools/docs/wiki/Texture-occupancy.md): union over every use of the image, following sampler filtering, wrapping and `KHR_texture_transform`. Each mesh counts once, and only triangles sample. `null` for an image without uses, and for one used by a primitive whose data can't be read. `texels.occupancy` is `null` if any used image's is. | Data |
| `texels.total` | Width × height of every image with at least one use, each image counted once. | Document |
| `diagnostics` | The same counted warnings as on stderr, with stable `code`s that consumers can map to localized messages. `message` is English only. See below. | — |

### Diagnostic codes

| Code | When |
| --- | --- |
| `texture-alternative-source` | A texture has a core `source` and an extension source; the core one is measured. Example: the texture. |
| `unknown-required-extension` | `extensionsRequired` lists an extension meshtinker doesn't know. The model is still inspected and imported. Example: the extension. |
| `image-unrecognized` | An image isn't PNG, JPEG, WebP, KTX2 or AVIF, or is truncated. |
| `draco-decode-failed` | Draco rejected a primitive's data, or it doesn't match its accessors: bounds, and occupancy of the images the primitive uses, are `null`. |
| `missing-vertex-data` | An accessor's buffer view holds no data: same consequences. |
| `meshopt-decode-failed` | meshoptimizer rejected compressed data: same consequences. |
| `missing-texture-coordinates` | A textured primitive lacks the `TEXCOORD_n` its texture needs: it is left out of occupancy. |
| `texture-repeats-skipped` | UV triangles repeating their texture more than 64 times vertically are left out of occupancy, as objtools does. |
| `invalid-texture-coordinates` | UV triangles with indices out of range or non-finite coordinates are left out of occupancy. |
| `extension-indices-not-renumbered` | `import` only: objects were dropped or renumbered while an unknown extension may hold indices to them. Example: the extension. |

## Without data (`--no-data`)

`inspect --no-data` reads the `.gltf`, or the GLB's header and JSON chunk, and opens nothing the model refers to. An upload route runs it on each `.gltf` or `.glb` as it arrives, to learn which other files to wait for (`requires`), whether they are there yet or not. A missing file isn't an error here. Its cost is parsing the JSON: 0.06 s and 11 MB on `MAN63675_HP_44M.gltf` (44 M faces, a 1.3 GB `.bin`), against 19 s and 1 GB for the full report.

References are checked from their text alone, with the rules of a full read: a URL, an absolute path, or a path whose `..` leads outside the model's folder (without `--trusted`) gets the answer a full read gives. A buffer is rejected with `reference-refused`, an image with `missing-image`. A symlink leading out can't be seen without the file, so the full read catches it later. So can a buffer shorter than its `byteLength`, a file that never arrives, or a corrupt one.

The report has every **JSON** field of the table above, with the same meaning, and none of the others: `"data": false` says which report it is. A key is left out rather than `null`, because `null` means a figure couldn't be determined (with a diagnostic), where here it wasn't looked for:

- `input.byteSize`; `input.files` holds the model file alone;
- `scene.bounds`;
- each image's `mimeType`, `width`, `height`, `byteSize` and `occupancy`;
- `texels`.

```jsonc
{
  "format": 1,
  "tool": { "name": "meshtinker-inspect", "version": "0.1.0" },
  "data": false,
  "input": { "files": [{ "role": "gltf", "name": "model.gltf", "byteSize": 20480 }] },
  "requires": [["model.bin"], ["diffuse.jpg"]],
  "extensions": { "used": [], "required": [] },
  "scene": { "index": 0, "name": "Scene", "firstNamedRootNode": "Cube" },
  "geometry": { "meshes": 1, "primitives": 1, "vertices": 24, "faces": 12,
                "primitivesByEncoding": { "plain": 1, "meshopt": 0, "draco": 0, "quantized": 0 } },
  "images": [
    { "index": 0, "name": "diffuse", "file": "diffuse.jpg",
      "uses": [{ "material": 0, "slot": "baseColorTexture", "texture": 0 }] }
  ],
  "diagnostics": []
}
```

Consumers type the two reports as one base and two extensions, told apart by `data`, so that the full report's fields stay required. In TypeScript, `if (report.data)` narrows to the full report:

```ts
interface ReportBase {
  format: number; tool: Tool; extensions: Extensions; geometry: Geometry;
  requires: string[][]; diagnostics: Diagnostic[];
  input: { files: File[] };
  scene: SceneBase | null;      // index, name, firstNamedRootNode
  images: ImageBase[];          // index, name, file, uses
}
interface NoDataReport extends ReportBase { data: false }
interface InspectReport extends ReportBase {
  data: true;
  input: { byteSize: number; files: File[] };
  scene: (SceneBase & { bounds: Bounds | null }) | null;
  images: (ImageBase & { mimeType: string | null; width: number | null; height: number | null;
                         byteSize: number; occupancy: number | null })[];
  texels: { total: number; occupancy: number | null };
}
```

With `--json`, a failure prints the same error object as a full read's.

## Mapping from eCorpus

| eCorpus field | From this report |
| --- | --- |
| `name` | `scene.firstNamedRootNode ?? scene.name ?? ""`. Without a `scene` property, eCorpus has no scene at all (name `""`, bounds `null`, vertex count 0), where the report uses scene 0. |
| `bounds` | `scene.bounds`. eCorpus ignores `EXT_mesh_gpu_instancing` instances. |
| `numFaces` | `geometry.faces`. eCorpus counts strips and fans as `floor(n / 3) − 2`, which is wrong; the figures differ on such files. eCorpus also counts meshes outside the default scene, which the profile drops. |
| `imageSize` | Maximum of `max(width, height)` over images with a `baseColorTexture` use. eCorpus measures a texture's WebP or KTX2 alternative instead of its core source. |
| `byteSize` | `input.byteSize` |
| `extensions` | `extensions.used` |
| warning `png-images` | Any image with `mimeType` `image/png` |
| warning `large-textures` | Any image with `max(width, height) > 4096` |
| warning `large-combined-diffuse-maps` | More than one image with a `baseColorTexture` use, and their total `width × height` above 4096² |
| optimizer's vertex count | `geometry.vertices` |

eCorpus can't read `KHR_meshopt_compression` files, and inspects files with unknown required extensions anyway.
