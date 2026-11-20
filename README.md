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
  compress      Compress the geometry of a glTF or GLB model with meshopt
  export        Write the last stage of a chain as a .gltf or .glb file
  import        Rewrite a glTF or GLB file in the normalized profile
  inspect       Report statistics about a glTF or GLB file
  placeholder   Build a low-poly stand-in for a glTF or GLB model
  simplify      Reduce the triangles of a glTF or GLB model

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

## Operations

### inspect

Reports statistics about a `.gltf` or `.glb` file, for the scene it shows (the default scene, or the first one), in text or, with `--json`, as the JSON object described in [docs/inspect-report.md](docs/inspect-report.md):

- every file the model consists of, with its size;
- the extensions it uses and requires;
- the scene: its name, its first named root node, and its exact world-space bounds;
- geometry: meshes, primitives, vertices (as uploaded to the GPU) and triangles, each mesh counted once however many nodes use it, and primitives by encoding (plain, meshopt, Draco, quantized);
- every image: format and size from its header, size in bytes, the material slots that use it, and its texture occupancy: the fraction of its texels a renderer actually samples, following [objtools' definition](https://github.com/sdumetz/objtools/wiki/Texture-occupancy).

Bounds read every vertex the scene draws, and occupancy rasterizes every UV triangle into a coverage map kept in a workspace file. meshopt, Draco and `KHR_mesh_quantization` data are decoded.

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

It writes `<output>.gltf` and `<output>.bin`, in the normalized profile. The report gives the source's face count, the grid (`resolution`, `cells`, `cellSize`, refit `passes`, `droppedParts`) and the output's vertices, faces and files.

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

### simplify

Reduces the triangles of every mesh with [meshoptimizer](https://github.com/zeux/meshoptimizer)'s simplifier, until its estimate of how far the surface moved reaches `--error`, a fraction of the scene's size (the longest side of its bounding box in world space). There is no target face count: each primitive loses what the error allows and no more, and one that can't lose anything is copied as it is. It keeps UV seams, and the borders primitives share, so that tiled scans don't crack, whether their tiles are primitives of one mesh or meshes of their own. Disconnected parts smaller than the error, such as floating scan noise, are removed, and a primitive that loses every triangle goes too (`primitive-removed`), with the material and images only it used. [docs/simplify.md](docs/simplify.md) says how, and how far the estimate can be from the actual distance.

Simplified primitives are written uncompressed (float32 attributes), as every command that rewrites geometry writes them; the others, compressed or not, are copied. Points and lines are copied with a warning (`points-and-lines-kept`). The report gives the error asked for and the largest one meshoptimizer reached, the primitives simplified, unchanged, removed and skipped, the input's and output's primitives, vertices and faces (each primitive once, however many nodes show it), and the files written.

A 44-million-face scan in 4 meshes takes about 70 s at the default error, down to 937,000 faces, with a heap under 5 MB.

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

### export

Writes the last stage of a [chain](#chains) as one `.gltf` file with its `.bin` and images beside it, or as one `.glb` file, by the output's extension. The model is written as the stage holds it, in the normalized profile. A `.glb` puts the images into its binary chunk after the buffer views, each in a buffer view of its own with its `mimeType`. A GLB can't exceed 4 GiB: a larger model fails with `glb-too-large` and can be written as `.gltf`. The output's files are copies (reflinks where the file system has them), never links to the chain's.

`--remove-chain` removes the chain once the output is published, under the chain's lock: `chain.json` first, then the stages and the lock. The chain's files then become the output's by hard link, rather than by copy. A directory that holds other files is kept, with them (`chain-directory-kept`).

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
          --remove-chain        Remove the chain once the output is written. Its directory stays
                                if it holds other files
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
