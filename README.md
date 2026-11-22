# Meshtinker

Perform various analysis and optimizations on glTF files, with an emphasis on low memory usage.

## Building

Requires CMake 3.20 and a C++17 compiler. Every dependency is vendored. Add `-DMESHTINKER_WARNINGS_AS_ERRORS=ON` to the first `cmake` to make warnings errors, as CI does.

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
build/meshtinker <command> [args...]
```

`build/` then holds the `meshtinker` wrapper and one `meshtinker-<command>` executable per command. Copy them to the same directory, or put each on `PATH`, to install them. They need only the system C library.

## Releases

Each [release](https://github.com/Holusion/meshtinker/releases) has Linux binaries for x86_64 and arm64, `meshtinker-linux-<arch>.tar.gz`, with a `bin/` folder to put on `PATH`, and their `SHA256SUMS`. They are built on Rocky Linux 8, and run on it and on later distributions, such as RHEL 8, Debian 10, Ubuntu 20.04 (natively or under WSL) and the `node:22-slim` image.

To publish one, set the version in `project()` in `CMakeLists.txt`, then push a tag `v<version>`, such as `v0.1.0`. CI builds and tests the binaries, and creates the release once every CI job has passed. A tag that doesn't match the version fails the build.

## Usage

```console
$ meshtinker --help
Analyse and optimize glTF 2.0 assets, using little memory.

Usage: meshtinker <command> [args...]
       meshtinker help <command>
       meshtinker --version

Commands:
  avif          Encode the images of a glTF or GLB model as AVIF
  compress      Compress the geometry of a glTF or GLB model with meshopt
  export        Write the last stage of a chain as a .gltf or .glb file
  import        Rewrite a glTF or GLB file in the normalized profile
  inspect       Report statistics about a glTF or GLB file
  ktx           Encode the images of a glTF or GLB model as KTX2
  normals       Write smooth normals where a glTF or GLB model has none or flat ones
  placeholder   Build a low-poly stand-in for a glTF or GLB model
  simplify      Reduce the triangles of a glTF or GLB model
  uv            Pack the UV charts of a glTF or GLB model tighter, and bake its textures
  weld          Merge identical vertices in a glTF or GLB model

Commands are the meshtinker-<command> executables next to meshtinker, then on PATH.
```

Every command follows the same conventions:

- Data goes to stdout, everything else (warnings, errors, progress) to stderr. Warnings are counted: one line per kind of problem, with up to three examples.
- Commands that report statistics print text, or one JSON object with `--json`. With `--json`, stdout holds a JSON object even when the command fails: it then has an `error` with a `code`, the `exitCode` and a `message`.
- Outputs are never overwritten, and appear only once the command succeeds. A failed or interrupted command leaves nothing behind.
- A model may only reference files inside its own folder, unless `--trusted` is given.
- Exit codes:

  | Code | Meaning |
  | ---- | ------- |
  | 0 | Success |
  | 1 | The input was rejected |
  | 2 | The command line is wrong |
  | 3 | A resource limit was reached: disk full, quota or file-size limit, out of memory |
  | 4 | Another problem with the machine: permissions, missing output directory, existing output |
  | 70 | Internal error: a bug in meshtinker |

### Chains

Commands can run one after another on a *chain*: a directory that keeps each command's result as a numbered stage, so that no file names need wiring.

```sh
meshtinker import scan.glb work/       # creates the chain, with stage 000
meshtinker simplify work/              # adds a stage built on the last one
meshtinker compress work/
meshtinker inspect work/               # reads the last stage
meshtinker export work/ scan-opt.glb   # writes the last stage as .glb or .gltf
```

- `import` creates a chain when its output isn't a `.gltf` file: a new directory, whose parent must exist, or an empty one. Any other command treats its input as a chain when it is a directory holding `chain.json`.
- Commands that change the model (`simplify`, `compress`) take no output in chain mode: they add a stage. Giving one is a command-line error (2). Without a chain they read and write files as before.
- `inspect` reads the last stage, and `export` writes it elsewhere. `placeholder` reads files only.
- A stage is `work/NNN-<command>-<id>/`, with the model under the source's name and `stage.json`: the command, its arguments, its JSON report, and `lossy`, the running total of lossy rounds the model has been through (the source's Draco or JPEG, simplification error, quantization). A command that quantizes geometry quantized already warns (`repeated-quantization`). Stages are never changed once published, and every one is kept until `export --remove-chain`.
- Files a stage would write identical to its parent's, such as the images of a geometry stage, are hard links to them rather than copies.
- One command at a time writes a stage: another one fails at once with exit code 4 (`chain-in-use`). Readers can run meanwhile.
- The workspace defaults to the chain's directory.

[docs/chaining.md](docs/chaining.md) has the layout and the reasons for it.

## Operations

### inspect

Reports statistics about a `.gltf` or `.glb` file, for the scene it shows (the default scene, or the first one), in text or, with `--json`, as the JSON object described in [docs/inspect-report.md](docs/inspect-report.md):

- every file the model consists of, with its size;
- the extensions it uses and requires;
- the scene: its name, its first named root node, and its exact world-space bounds;
- geometry: meshes, primitives, vertices (as uploaded to the GPU) and triangles, each mesh counted once however many nodes use it, and primitives by encoding (plain, meshopt, Draco, quantized);
- every image: format and size from its header, size in bytes, the material slots that use it, and its texture occupancy: the fraction of its texels a renderer actually samples, following [objtools' definition](https://github.com/sdumetz/objtools/wiki/Texture-occupancy).

Bounds read every vertex the scene draws, and occupancy rasterizes every UV triangle into a coverage map kept in a workspace file. meshopt, Draco and `KHR_mesh_quantization` data are decoded.

Given a [chain](#chains), `inspect` reports on its last stage, without taking the chain's lock.

```console
$ meshtinker inspect --help
Report statistics about a glTF or GLB file

Sizes, geometry, bounds, images and texture occupancy of the scene the file
shows. Only the default scene counts, as `import` keeps only that one. Given a
chain, reads its last stage.


meshtinker inspect [OPTIONS] input


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file, or a chain: its last stage

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the chain, else
                                the input's directory)
          --keep-tmp            Keep the temporary workspace and print its path, for debugging
```

### import

Rewrites a `.gltf` or `.glb` file in the [normalized profile](docs/normalized-profile.md). Every other command except `inspect` normalizes its input the same way in memory and writes the profile, so running `import` first is optional; it makes later commands read their input front to back. Importing an imported file writes identical files. The profile is:

- one `.gltf`, one `.bin` and the images as separate files;
- only the default scene and what it uses;
- geometry and image encodings (meshopt, Draco, quantization, WebP, KTX2), unknown extensions and `extras` kept as they are.

`import` reports the same statistics as `inspect`, for the files it wrote. Both run the same pipeline, so they accept and reject the same files, and `inspect` on `import`'s output gives `import`'s report.

Given a directory instead of a `.gltf` output, `import` creates a [chain](#chains) there, with the model as stage 000 and its report in `stage.json`. A directory that exists must be empty; one that `import` created is removed if it fails. Stage 000 records in `lossy` what the source was already through: Draco or `KHR_mesh_quantization` geometry, and JPEG, WebP and KTX2 images. Image files and a `.bin` that `import` would write unchanged are copied with a reflink where the file system has one, and never linked to the source.

```console
$ meshtinker import --help
Rewrite a glTF or GLB file in the normalized profile

Writes the output .gltf, one .bin beside it and the images as files, keeping
only the default scene. Geometry and image encodings, unknown extensions and
extras are kept. Prints the same statistics as `inspect`, measured on what was
written. Given a directory, creates a chain there, with the model as its first
stage.


meshtinker import [OPTIONS] input output


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file
  output TEXT REQUIRED          The .gltf file to write, or a chain to create: a directory, new
                                or empty. Its parent must exist; no file is overwritten

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the chain, else
                                the output's directory)
          --keep-tmp            Keep the temporary workspace and print its path, for debugging
```

### placeholder

Builds a low-poly, untextured stand-in for a model, to display while the model is processed. It overlays what the default scene shows at rest, and its size depends on `--resolution` and the model's shape, not on the face count: a few hundred to a few thousand faces at the default resolution of 16, and about 58,000 at 128 for a 44-million-face scan. That scan takes a few seconds, with a heap under 20 MB.

The placeholder is deliberately approximate. It may cut slightly into the model, and bevels sharp edges by about one cell. Enclosed interior geometry is left out, holes stay open down to a few cells, and open surfaces become thin sheets. Small parts far from the model, such as scan noise, are left out with a warning (`small-parts-left-out`), and so are points and lines. A part that fits inside a single cell disappears, so a scene of objects far apart may need a higher `--resolution`. [docs/placeholder.md](docs/placeholder.md) explains how it is built and where it sits.

It writes `<output>.gltf` and `<output>.bin`, in the normalized profile. It reads and writes files only, not [chains](#chains): its output is not the next version of its input. The report gives the source's face count, the grid (`resolution`, `cells`, `cellSize`, refit `passes`, `droppedParts`) and the output's vertices, faces and files.

```console
$ meshtinker placeholder --help
Build a low-poly stand-in for a glTF or GLB model

Voxelizes the faces the default scene shows at rest, then writes the outer
surface of the occupied cells as one untextured mesh, placed to overlay the
model. Its size depends on the resolution and the model's shape, not on its face
count.


meshtinker placeholder [OPTIONS] input output


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file
  output TEXT REQUIRED          The .gltf file to write. Its directory must exist; no file is
                                overwritten

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --resolution N        Cells along the model's longest axis, 2 to 128 (default 16)
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the output's
                                directory)
          --keep-tmp            Keep the temporary workspace and print its path, for debugging
```

### compress

Quantizes and compresses the geometry with `EXT_meshopt_compression`, in the preset of glTF-Transform's `meshopt({level: "high"})`: 14-bit positions, 8-bit octahedral normals, 12-bit texture coordinates, 8-bit colors and weights, with primitives reordered for the vertex cache first. Positions go on one grid for the whole scene, so that the tiles of a scan quantize their shared borders the same way, and the grid's scale goes into the node transforms, or a new child node when the node's transform can't take it. Skinned, instanced and morphed meshes keep float positions. Draco primitives are copied as they are, with a warning (`draco-kept`), and so are primitives already compressed with meshopt, so compressing twice changes nothing. [docs/compress.md](docs/compress.md) has the details and figures.

The report gives the primitives compressed and kept, the meshes whose positions were quantized or kept float, the nodes added, the bytes of the geometry's buffer views before and after, and the files written. A 44-million-face scan's 1.32 GB of geometry becomes 207 MB in 20 s, with a heap under 6 MB.

Given a [chain](#chains), `compress` adds a stage, and records its quantization in `lossy.geometryQuantization`, with the bits of positions, normals and texture coordinates. It warns `repeated-quantization` when an earlier stage, or the source, quantized geometry already: a chain from a Draco source through `simplify` quantizes twice. The images are links to the parent stage's.

```console
$ meshtinker compress --help
Compress the geometry of a glTF or GLB model with meshopt

Reorders, quantizes and encodes every primitive with EXT_meshopt_compression, as
glTF-Transform's meshopt "high" level does: 14-bit positions, 8-bit octahedral
normals, 12-bit texture coordinates. Draco and meshopt primitives are copied as
they are.


meshtinker compress [OPTIONS] input [output]


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file, or a chain: its last stage
  output TEXT                   The .gltf file to write. Its directory must exist; no file is
                                overwritten. Not given when INPUT is a chain: the command adds a
                                stage to it

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the chain, else
                                the output's directory)
          --keep-tmp            Keep the temporary workspace and print its path, for debugging
```

### normals

Writes smooth normals on the triangle primitives that have no `NORMAL`, or only flat ones: the three normals of every triangle equal, one per face. Both are usually a mistake made upstream, and normals default to smooth ([AGENTS.md](AGENTS.md#constraints)): most local scans have no normals, which the spec renders flat, and some store one normal per face. Primitives with their own normals, smooth or split along authored hard edges, are kept as they are, and so are flat ones with `--keep-flat`.

Normals are summed over positions, not vertices, so UV seams leave no crease. They are also summed across the primitives that share a border and are drawn with the same transforms, so tiles leave no seam. Each face's normal is weighted by its corner's angle, as Blender does. Edges whose faces meet at more than `--crease-angle` (30° by default, as in Blender) stay hard: boxes and walls stay square, while noisy scans look more faceted than with a larger angle. A vertex on both sides of a hard edge is split in two. Every vertex at one position, on one side of its hard edges, gets the same normal, bit for bit, so that `weld` can merge them afterwards: run `normals` before `weld`. [docs/normals.md](docs/normals.md) says how, and compares 30° with 80°.

Rewritten primitives are written uncompressed (float32 attributes), as every command that rewrites geometry writes them, and lose their `TANGENT`: tangents follow the normals they were made with, and viewers compute their own when there are none. Primitives with an unlit material (`KHR_materials_unlit`) are left without normals, since no viewer shades them. Points and lines are copied as they are. Primitives with morph targets are copied with a warning (`morph-targets-kept`), as are those that can't be read (`normals-not-written`). The report gives the crease angle, the primitives whose normals were added, smoothed, kept, unlit or skipped, the vertices and faces of the rewritten primitives before and after, and the files written.

Given a [chain](#chains), `normals` adds a stage, and records no lossy round. The images are links to the parent stage's.

```console
$ meshtinker normals --help
Write smooth normals where a glTF or GLB model has none or flat ones

Computes smooth normals for the triangle primitives that have no NORMAL, or one
normal per face, summed over positions and across the primitives that share
them, with edges sharper than the crease angle kept hard. Authored normals are
kept. Rewritten primitives are written uncompressed.


meshtinker normals [OPTIONS] input [output]


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file, or a chain: its last stage
  output TEXT                   The .gltf file to write. Its directory must exist; no file is
                                overwritten. Not given when INPUT is a chain: the command adds a
                                stage to it

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --crease-angle DEGREES 
                                Edges whose faces meet at more than this angle, in degrees, stay
                                hard: 0 keeps every edge hard, 180 none (default 30)
          --keep-flat           Keep flat normals (one per face) as they are: only primitives
                                without normals get them
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the chain, else
                                the output's directory)
          --keep-tmp            Keep the temporary workspace and print its path, for debugging
```

### weld

Merges the vertices of each triangle primitive that are equal in every attribute and morph target, bit for bit, with −0 equal to 0, as glTF-Transform's `weld` does. Every triangle still draws the same values, so nothing changes for a viewer that follows the spec. Scans written three vertices per triangle, or with duplicated vertices, shrink most: by 81 % on a 4.7-million-face scan. And `simplify` can only collapse edges between vertices that are shared. Vertices are never moved or averaged: two vertices at one position with different normals or texture coordinates stay apart. [docs/weld.md](docs/weld.md) says why, and how Voyager draws welded primitives that have no normals.

Welded primitives are written uncompressed (float32 attributes), as every command that rewrites geometry writes them, and primitives where nothing merges are copied as they are. Draco and meshopt primitives are copied: their encoders already merged their vertices. Primitives that share vertex data with another primitive are copied with a warning (`shared-vertices-kept`), as are points and lines (`points-and-lines-kept`). The report gives the primitives welded, unchanged, compressed and skipped, the input's and output's primitives, vertices and faces, and the files written.

Given a [chain](#chains), `weld` adds a stage, and records no lossy round. The images are links to the parent stage's.

```console
$ meshtinker weld --help
Merge identical vertices in a glTF or GLB model

Merges the vertices of each triangle primitive that are equal in every attribute
and morph target, bit for bit. Welded primitives are written uncompressed; Draco
and meshopt primitives, and those that share vertices or can't be read, are
copied as they are.


meshtinker weld [OPTIONS] input [output]


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file, or a chain: its last stage
  output TEXT                   The .gltf file to write. Its directory must exist; no file is
                                overwritten. Not given when INPUT is a chain: the command adds a
                                stage to it

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the chain, else
                                the output's directory)
          --keep-tmp            Keep the temporary workspace and print its path, for debugging
```

### simplify

Reduces the triangles of every mesh with [meshoptimizer](https://github.com/zeux/meshoptimizer)'s simplifier, until its estimate of how far the surface moved reaches `--error`, a fraction of the scene's size (the longest side of its bounding box in world space). There is no target face count: each primitive loses what the error allows and no more, and one that can't lose anything is copied as it is. It keeps UV seams, and the borders primitives share, so that tiled scans don't crack, whether their tiles are primitives of one mesh or meshes of their own. Disconnected parts smaller than the error, such as floating scan noise, are removed, and a primitive that loses every triangle goes too (`primitive-removed`), with the material and images only it used. [docs/simplify.md](docs/simplify.md) says how, and how far the estimate can be from the actual distance.

Simplified primitives are written uncompressed (float32 attributes), as every command that rewrites geometry writes them; the others, compressed or not, are copied. Points and lines are copied with a warning (`points-and-lines-kept`). The report gives the error asked for and the largest one meshoptimizer reached, the primitives simplified, unchanged, removed and skipped, the input's and output's primitives, vertices and faces (each primitive once, however many nodes show it), and the files written.

A 44-million-face scan in 4 meshes takes about 70 s at the default error, down to 937,000 faces, with a heap under 5 MB.

Given a [chain](#chains), `simplify` adds a stage, and adds the largest error it reached to `lossy.simplificationError`, so that the total over several runs stays known. The images are links to the parent stage's.

```console
$ meshtinker simplify --help
Reduce the triangles of a glTF or GLB model

Collapses edges with meshoptimizer until its estimate of how far the surface
moved reaches the error, keeping UV seams and the borders primitives share.
Simplified primitives are written uncompressed, emptied ones are removed, and
others are copied as they are.


meshtinker simplify [OPTIONS] input [output]


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file, or a chain: its last stage
  output TEXT                   The .gltf file to write. Its directory must exist; no file is
                                overwritten. Not given when INPUT is a chain: the command adds a
                                stage to it

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --error E             How far the surface may move, as a fraction of the scene's size
                                (default 0.001)
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the chain, else
                                the output's directory)
          --keep-tmp            Keep the temporary workspace and print its path, for debugging
```

### ktx

Encodes the scene's PNG and JPEG images as KTX2 with Basis Universal and points their textures at them through `KHR_texture_basisu`, as eCorpus does with `ktx create` through glTF-Transform. Color slots (base color, emissive, sheen and specular color, spec-gloss diffuse and specular) get ETC1S in sRGB; normal maps, ORM and every other data slot get UASTC in linear, with Zstandard. An image whose alpha is all 255, or that only opaque materials sample, is written without alpha. Every image gets a full mip chain, and sides that aren't multiples of 4 are rounded up, as the extension requires. The textures' other sources go: the PNG or JPEG, and any `EXT_texture_webp` or `EXT_texture_avif` alternative. `KHR_texture_basisu` is then required. Images are left as they are, each with a counted warning, when they are WebP or AVIF only (`image-format-unsupported`), already KTX2 (`image-already-compressed`), used as color by one slot and as data by another (`image-color-space-conflict`), used only by slots `ktx` doesn't know (`image-slot-unknown`), or can't be decoded. [docs/ktx.md](docs/ktx.md) has the details, and the benchmark that set the presets.

`--quality` takes eCorpus's names: ETC1S quality levels 250, 240 (`High`, the default), 220, 180 and 128; UASTC is level 1 at every quality. `--max-size N` scales down images whose longest side is over N texels; without it, `ktx` doesn't resize. Large images are encoded one at a time on every CPU the process may use (its affinity, capped by the cgroup's `cpu.max`), and images under a megatexel several at a time. DamagedHelmet's five 2k images take 16 s with a 365 MiB peak, a 4k scan texture 15 s and 1.1 GB.

The report gives, per image, its source format and size, codec, transfer function, whether alpha was kept, the output's size, bytes before and after, and the time; then the totals and the files written. Given a [chain](#chains), `ktx` adds a stage and records one `lossy.imageEncoding` round per image (`codec`, and `qualityLevel` or `uastcLevel`). It warns `repeated-lossy-image-encoding` when an earlier stage recorded one, such as `import` for a JPEG source. Run `uv` and other image commands before it: they leave KTX2 images as they are.

```console
$ meshtinker ktx --help
Encode the images of a glTF or GLB model as KTX2

Encodes PNG and JPEG images with Basis Universal and references them through
KHR_texture_basisu: ETC1S for color, UASTC for normal maps and other data. The
textures' other sources are removed.


meshtinker ktx [OPTIONS] input [output]


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file, or a chain: its last stage
  output TEXT                   The .gltf file to write. Its directory must exist; no file is
                                overwritten. Not given when INPUT is a chain: the command adds a
                                stage to it

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --quality <Highest|High|Medium|Low|Thumb> 
                                ETC1S quality, as eCorpus's presets name it (default High)
          --max-size N          Scale down images whose longest side is over N texels, keeping
                                their aspect ratio
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
```

### avif

Encodes the scene's PNG and JPEG images as AVIF with libavif and libaom, and points their textures at them through `EXT_texture_avif`. It's the web-image alternative to `ktx`: smaller downloads, but textures that decode to RGBA8 on the GPU. Color slots get 4:2:0, normal maps, ORM and every other data slot 4:4:4, gray images 4:0:0. Every file gets the sRGB color description, which browsers leave as it is. An image whose alpha is all 255, or that only opaque materials sample, is written without alpha. The textures' other sources go: the PNG or JPEG, and any `EXT_texture_webp` alternative. `EXT_texture_avif` is then required, so browsers without AVIF (Safari and iOS before 16, Edge before 121) can't load the model. KTX2 textures are kept.

Some images are left as they are:
- An image whose AVIF isn't smaller than its source keeps its source; tiny placeholders are smaller as PNG.
- With a counted warning: WebP-only textures (`image-format-unsupported`), images used only by slots `avif` doesn't know (`image-slot-unknown`), and images that can't be decoded.

[docs/avif.md](docs/avif.md) has the details, and [docs/experiments/webp-avif.md](docs/experiments/webp-avif.md) the comparison that chose AVIF and set the presets.

`--quality` takes eCorpus's names: libavif quality 80, 65 (`High`, the default), 50, 40 and 30. `--effort` is sharp's scale, 0 (fastest) to 9:
- the default, 3, is within 10 % of the slowest effort's size at a quarter of effort 4's time;
- efforts 0 and 1 inflate some images.

`--max-size N` scales down images whose longest side is over N texels. Large images are encoded one at a time on every CPU the process may use, small ones several at a time, as `ktx` does. A 4k scan texture takes 3.4 s with a 297 MiB peak, three 8k textures 25 s and 1.6 GiB.

The report gives, per image, its source format and size, pixel format, whether alpha was kept, the output's size, bytes before and after, and the time. Then come the totals, the images kept because they were smaller (`notSmaller`), and the files written. Given a [chain](#chains), `avif` adds a stage and records one `lossy.imageEncoding` round per image (`quality`, `effort`, `subsampling`). Run `uv` and other image commands before it: they leave AVIF images as they are.

The build downloads and compiles libavif, libaom and nasm ([vendor/README.md](vendor/README.md#downloaded-at-build-time)). Configure with `-DMESHTINKER_AVIF=OFF` to build without them, and without `avif`.

```console
$ meshtinker avif --help
Encode the images of a glTF or GLB model as AVIF

Encodes PNG and JPEG images with libavif and libaom and references them through
EXT_texture_avif, which becomes required: color in 4:2:0, normal maps and other
data in 4:4:4. The textures' other sources are removed; KTX2 textures are kept.


meshtinker avif [OPTIONS] input [output]


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file, or a chain: its last stage
  output TEXT                   The .gltf file to write. Its directory must exist; no file is
                                overwritten. Not given when INPUT is a chain: the command adds a
                                stage to it

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --quality <Highest|High|Medium|Low|Thumb> 
                                Quality, as eCorpus's presets name it: libavif quality 80, 65,
                                50, 40, 30 (default High)
          --effort E:INT in [0 - 9] 
                                Encoder effort, 0 (fastest) to 9, as sharp counts it; 0 and 1
                                inflate some images (default 3)
          --max-size N          Scale down images whose longest side is over N texels, keeping
                                their aspect ratio
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
```

### uv

Packs the UV charts of each material tighter and bakes its textures into the new layout, so that fewer texels are wasted: `inspect`'s texture occupancy is the measure. Charts are only moved, by whole texels, so baking copies texels without resampling, and only the texture coordinates of the material's primitives change (no vertex is added; the primitives are written uncompressed). Materials that share images are packed together, and every image of a material gets the same layout, keeping their ratio of sizes. The baked images are PNG; run `ktx` after `uv`. A material whose images wouldn't lose 10 % of their texels is left as it is. So is, with a counted warning, a material with KTX2, WebP or AVIF images, a texture transform, textures on different texture coordinate sets, a repeating texture its UVs tile, morphed texture coordinates, or images that aren't whole multiples of each other in size. [docs/uv.md](docs/uv.md) has the details.

`--max-size N` scales down baked images whose longest side is over N texels. The report gives, per group of materials, its status, charts, the images' sizes before and after, texels and occupancy before and after, and the time. Given a [chain](#chains), `uv` adds a stage; a PNG adds no lossy round.

```console
$ meshtinker uv --help
Pack the UV charts of a glTF or GLB model tighter, and bake its textures

Moves the UV charts of each material closer together, by whole texels, and
copies their texels into smaller images, written as PNG. Charts are not rotated,
scaled or re-cut. Materials whose images wouldn't shrink by 10 % are left as
they are.


meshtinker uv [OPTIONS] input [output]


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file, or a chain: its last stage
  output TEXT                   The .gltf file to write. Its directory must exist; no file is
                                overwritten. Not given when INPUT is a chain: the command adds a
                                stage to it

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --max-size N          Scale down images whose longest side is over N texels, keeping
                                their aspect ratio
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the chain, else
                                the output's directory)
          --keep-tmp            Keep the temporary workspace and print its path, for debugging
```

### export

Writes the last stage of a [chain](#chains) as one `.gltf` file with its `.bin` and images beside it, or as one `.glb` file, by the output's extension. The model is written as the stage holds it, in the normalized profile. A `.glb` puts the images into its binary chunk after the buffer views, each in a buffer view of its own with its `mimeType`. A GLB can't exceed 4 GiB: a larger model fails with `glb-too-large` and can be written as `.gltf`. The output's files are copies (reflinks where the file system has them), never links to the chain's.

`--remove-chain` removes the chain once the output is published, under the chain's lock: `chain.json` first, then everything else in the chain's directory, other files included, and the directory itself. The chain's files then become the output's by hard link, rather than by copy. An output inside the chain's directory is refused (exit code 2). A directory that can't be removed, such as a mount point, is left empty, with a warning (`chain-directory-kept`).

The report gives the stage written and the files, with their sizes.

```console
$ meshtinker export --help
Write the last stage of a chain as a .gltf or .glb file

A .gltf output gets its .bin and images beside it, as in the stage. A .glb
output holds everything: the images go into its binary chunk. The model is
written as the stage holds it.


meshtinker export [OPTIONS] input output


POSITIONALS:
  input TEXT REQUIRED           The chain: its last stage
  output TEXT REQUIRED          The .gltf or .glb file to write. Its directory must exist; no
                                file is overwritten

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --remove-chain        Remove the chain's directory, with everything in it, once the
                                output is written
          --json                Print the result as one JSON object on stdout, even on failure
```

## Architecture

The tool will be used to test a (possibly large) number of algorithms to perform a lot of optimizations. In some case we'll want to have more than one algorithm for one task.

However we want to have all those subcommands under the same "umbrella".

For this we favor an approach where each subcommand gets built into its own binary, with a small wrapper that serves as a master entrypoint.

The idea is that it should be easy for someone to try out any number of subcommands for multiple experimental branches at the same time (as long as their names don't overlap). It looks a lot like git is structured, so that may be a good inspiration.

code-wise, we should make it easy for subcommands to share common code, but builds should be mostly static: We don't want to ship a bunch of shared libraries along with the binaries. Any non-static dependency need to be carefully considered before inclusion. 

Cross-platform compatibility is not to be taken too seriously. We anticipate 90% linux, 10% WSL and MacOS use. We shouldn't willingly make it harder than required to build and run the tools on windows if it's ever needed, but we shouldn't over-provision for it until then.

See [AGENTS.md](AGENTS.md#architecture) for how subcommands are found and built.
