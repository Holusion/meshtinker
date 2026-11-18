# Meshtinker

Perform various analysis and optimizations on glTF files, with an emphasis on low memory usage.

## Building

Requires CMake 3.20 and a C++17 compiler. Every dependency is vendored.

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build
build/meshtinker <command> [args...]
```

`build/` then holds the `meshtinker` wrapper and one `meshtinker-<command>` executable per command. Copy them to the same directory, or put each on `PATH`, to install them. They need only the system C library.

## Usage

```console
$ meshtinker --help
Analyse and optimize glTF 2.0 assets, using little memory.

Usage: meshtinker <command> [args...]
       meshtinker help <command>
       meshtinker --version

Commands:
  import        Rewrite a glTF or GLB file in the normalized profile
  inspect       Report statistics about a glTF or GLB file
  placeholder   Build a low-poly stand-in for a glTF or GLB model

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
shows. Only the default scene counts, as `import` keeps only that one.


meshtinker inspect [OPTIONS] input


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the input's
                                directory)
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
written.


meshtinker import [OPTIONS] input output


POSITIONALS:
  input TEXT REQUIRED           The .gltf or .glb file
  output TEXT REQUIRED          The .gltf file to write. Its directory must exist; no file is
                                overwritten

OPTIONS:
  -h,     --help                Print this help message and exit
          --version             Print the version and exit
          --json                Print the result as one JSON object on stdout, even on failure
          --trusted             Let the model reference files outside its folder: absolute paths,
                                `..` leading out, symlinks
          --tmp-dir TEXT:DIR    Where to create the temporary workspace (default: the output's
                                directory)
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

## Architecture

The tool will be used to test a (possibly large) number of algorithms to perform a lot of optimizations. In some case we'll want to have more than one algorithm for one task.

However we want to have all those subcommands under the same "umbrella".

For this we favor an approach where each subcommand gets built into its own binary, with a small wrapper that serves as a master entrypoint.

The idea is that it should be easy for someone to try out any number of subcommands for multiple experimental branches at the same time (as long as their names don't overlap). It looks a lot like git is structured, so that may be a good inspiration.

code-wise, we should make it easy for subcommands to share common code, but builds should be mostly static: We don't want to ship a bunch of shared libraries along with the binaries. Any non-static dependency need to be carefully considered before inclusion. 

Cross-platform compatibility is not to be taken too seriously. We anticipate 90% linux, 10% WSL and MacOS use. We shouldn't willingly make it harder than required to build and run the tools on windows if it's ever needed, but we shouldn't over-provision for it until then.

See [AGENTS.md](AGENTS.md#architecture) for how subcommands are found and built.
