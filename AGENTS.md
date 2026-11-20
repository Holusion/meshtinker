# Goals

`meshtinker` is a set of command-line tools that analyse and optimize glTF 2.0 assets, often photogrammetry scans with large geometry and textures, while using little memory. Every tool runs under one `meshtinker` entry point. Many tools are expected, some of them competing experimental algorithms for the same task.

The README holds the command reference. `docs/` holds behaviour and design notes, starting with [the normalized profile](docs/normalized-profile.md) that `import` writes, and that every other command except `inspect` reads any input as, in memory, and writes ([how](docs/normalized-profile.md#how-commands-get-normalized-input)).

# Constraints

- **Map, don't load.** Buffers and images are memory-mapped read-only and read front to back where possible. The one accepted exception is Draco, whose decoder decodes a whole primitive into memory. The heap holds the glTF JSON document and small per-object records (per mesh, primitive, material, image), never per-vertex, per-face or per-texel data for a whole model. Large working sets, such as texture coverage maps, go in memory-mapped workspace files. Disk spools as in `mesh2gltf` are not needed: glTF input is already binary and can be indexed in place.
- **No memory limits of our own.** Commands don't cap their memory use or refuse inputs for their size: the caller (a container, a cgroup, `ulimit`) sets limits, and kills a run that goes over. A failed allocation exits with 3 (`out-of-memory`). Data URIs and Draco primitives are the known cases that grow the heap with the input.
- **Workspaces go on a real disk.** They default to the output's directory (the input's for read-only commands), not the OS temporary directory, which is often RAM-backed. `--tmp-dir` overrides it; `--keep-tmp` preserves it for debugging.
- **Don't downgrade, don't strip.** Keep meshopt and Draco geometry, WebP and KTX2 textures, unknown extensions and `extras` as they are. A command changes only what its documentation says it changes. The one exception is geometry a command rewrites: it is written uncompressed, and compression is left to its own command ([normalized-profile.md](docs/normalized-profile.md#commands-that-rewrite-geometry)).
- **Real files are messy.** Prefer one counted warning (with up to three examples) for unsupported data over aborting. Reserve errors for input that can't be interpreted, or that would silently give wrong results.
- **Interop choices are credited.** When behaviour follows another implementation (three.js, Blender, glTF-Transform, a spec section), name it in the code comment or document. That way the choice can be traced, and later swapped.
- **Sizes are 64-bit.** A `.bin` can exceed 4 GiB; read and write lengths and offsets as 64-bit values.

# Architecture

Each subcommand is its own executable, `meshtinker-<name>`, in the style of git. The `meshtinker` wrapper finds and runs them:

- `meshtinker <name> [args...]` looks for `meshtinker-<name>` in the wrapper's own directory, then on `PATH`, and runs the first match with the remaining arguments (`exec` on POSIX, spawn and wait elsewhere), passing its exit code through.
- `meshtinker --help` prints its usage and lists the subcommands it finds, with a one-line summary for each. `meshtinker` alone prints the same on stderr and exits with 2, the code for a wrong command line, as GNU tools do: 1 would read as a rejected input. Every subcommand prints its summary when given a hidden `--summary` flag (`cli::Command` handles it). `meshtinker help <name>` is `meshtinker-<name> --help`.
- An experimental variant is a subcommand with its own name, such as `meshtinker-simplify-qem`. It can be built from another branch or checkout and put on `PATH` next to the others.
- Each optimization (weld, simplify, meshopt compression, texture compression...) is its own subcommand. Meta-subcommands chain them into sensible defaults, such as eCorpus's quality presets, by running the individual subcommands.

Builds are mostly static: vendored libraries are compiled in, and libstdc++ and libgcc are linked statically. Only the system C library is linked dynamically. Any other shared-library dependency, or any external program called at runtime, needs explicit approval.

Platform priority is Linux, then WSL and macOS. Don't make Windows harder than it needs to be, but don't build for it either.

# Code layout

C++17, built with CMake from the repository root. The build scaffolding comes from `../cluster_lod/packages/tools`.

- `include/meshtinker/<namespace>/`, `src/<namespace>/` and `test/<namespace>/` match C++ namespaces under `meshtinker`.
- Shared namespaces build into the `meshtinker-core` static library: `common` (files, mappings, workspaces, outputs, exit codes, diagnostics), `gltf` (document reading and writing, image headers, the index graph, scene selection and layout, geometry decoding, node transforms), `cli` (option and help helpers shared by every subcommand), `analysis` (the inspection report and the pipeline `inspect` and `import` share, texture coverage).
- Each subcommand lives in `src/commands/<name>/` (namespace `meshtinker::<name>`), with its `main.cpp`, and builds into `meshtinker-<name>`. The wrapper lives in `src/commands/meshtinker/`.
- Shared namespaces never depend on subcommand namespaces. Code moves to a shared namespace once a second subcommand needs it.
- `vendor/` holds third-party code (cgltf, CLI11, Draco's decoder, meshoptimizer, nlohmann/json, stb), each with its license and version in `vendor/README.md`. It builds as a separate target without project warnings.

# Command-line conventions

- Options are declared once, with their help text, through CLI11, in a `cli::Command` (`include/meshtinker/cli/command.hpp`). It also holds the `input` and `output` positionals (with the output directory check and the workspace's default location) and the standard options (`--json`, `--trusted`, `--tmp-dir`, `--keep-tmp`), so their names and wording are the same everywhere, and `Command::run` handles parsing, `--summary`, exit codes and error reports. `--help` is generated from those declarations, and the option blocks in the README are copied from `--help` output; `test/cli/readme_help_test.cpp` checks that they still match and prints the block to paste when they don't.
- Stdout carries only a command's data output. Everything else (progress, warnings, errors, summaries) goes to stderr.
- Commands that report statistics print text by default and JSON with `--json`. Both are rendered from the same result structure, and JSON is the source of truth. With `--json`, stdout is one JSON object even when the command fails: it then holds an `error` and the diagnostics gathered so far. The `inspect` report's shape is documented in [docs/inspect-report.md](docs/inspect-report.md), and consumers generate types from it.
- Counted warnings go to stderr and into the JSON `diagnostics`, with a stable `code`. Messages are English only. Localization, if ever needed, belongs to JSON consumers, who map codes.
- Outputs are never overwritten, and the destination directory must already exist. A failed run removes only the files it created.
- A file reference in a model (buffer or image `uri`) must lead to a regular file inside the input's folder. `..` is allowed only while the path stays inside that folder. Absolute paths, URLs and symlinks leading out are refused. Reading outside the folder requires `--trusted`. Messages show file names, not their folders.
- Exit codes:

  | Code | Meaning |
  | ---- | ------- |
  | 0 | Success |
  | 1 | The input was rejected |
  | 2 | The command line is wrong |
  | 3 | A resource limit was reached: disk full, quota or file-size limit, out of memory |
  | 4 | Another problem with the machine: permissions, missing output directory, existing output |
  | 70 | Internal error |

# Conventions

- Comments and docs describe current behaviour and the reasons for it. They don't describe development history or removed features.
- Every behaviour change comes with tests and a documentation update: the README for options and reported statistics, `docs/` for behaviour and implementation details.
- Project code builds with the strict warning set from cluster_lod (`-Wconversion -Wsign-conversion` and the rest) as errors. Write narrowing conversions as explicit casts after checking the range.
- Before committing, run:

  ```sh
  cmake -S . -B build && cmake --build build && ctest --test-dir build
  cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DMESHTINKER_SANITIZE=ON
  cmake --build build/asan && ctest --test-dir build/asan
  ```

- clang-tidy is not part of the pre-commit checks. It runs in CI, and locally through the `tidy` build target, which lints project sources (not `vendor/`) with `.clang-tidy`. The configuration comes from cluster_lod: bug-prone, performance and a few narrowing checks, and no style-only checks. Run it when a change does offset or size arithmetic. Its widening checks catch 32-bit products that overflow before they become 64-bit offsets, which tests with small files never trigger. CI uses the clang-tidy 20 that comes with its ubuntu-26.04 runner image, one version ahead of the 19 used locally: it has a few more `bugprone` checks, so CI can report findings that a local run doesn't.

  ```sh
  cmake --build build --target tidy
  ```

- [docs/validation.md](docs/validation.md) says how to check a command's output: which models to use (`test/corpus.json`, selected by tag with `tools/corpus.mjs`), which tool answers which question (validator, semantic comparison, reference figures and snapshots, memory), and the known differences with each reference tool. For output changes, at least validate the output against its input and compare the two. Measure memory on the `large` models when a change touches how data is read or written.
- ctest uses only the fixtures in `test/data`, with their `expected.json` (authoritative). CI has no other models but the pinned Khronos samples, in the end-to-end workflow. A new case gets a fixture: see docs/validation.md.
