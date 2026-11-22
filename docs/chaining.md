# Chaining commands

Commands can be run one after another on a *chain*: a directory that holds each step's result as a numbered *stage*. A user chains commands by hand with no file names to wire, and meta-commands such as `optimize` chain them the same way. The README documents the commands; this is how chains work and why. The code is `include/meshtinker/chain/` (records, stages, the lock), `cli::Command` (positionals by command kind) and `common::Outputs` (publishing, linking).

```bash
meshtinker import foo.glb work/          # output is a directory: creates the chain, stage 000
meshtinker simplify work/                # input is a chain, no output: adds a stage built on the last one
meshtinker compress work/
meshtinker inspect work/                 # reads the last stage
meshtinker export work/ out.glb          # writes the last stage as GLB or .gltf
```

File mode stays as it is: `meshtinker simplify in.gltf out.gltf` reads and writes plain files, with no chain. eCorpus and experimental variants can keep using it.

## Layout

```
work/
  chain.json                 {format, source: "foo.glb", created}
  lock                       held with flock while a command writes a stage
  000-import-k3v9q/   foo.gltf foo.bin foo_texture_0.png  stage.json
  001-simplify-7xd2m/ foo.gltf foo.bin foo_texture_0.png  stage.json
  002-compress-p0a4c/ ...
```

- **The chain is a directory the user names.** It is not looked up from the input file's path in a cache directory, which would break on two pipelines over one file, relative paths and symlinks, and would hide state that nothing cleans. It is not created beside the source either, since source folders may be read-only or must not be touched. It goes where the user puts it, on a real disk ([files-and-memory.md](files-and-memory.md#workspaces)). `import` always takes the chain directory as its output: there is no default location. An output that doesn't end in `.gltf` is a chain (`.glb` is a command-line error, since `import` writes the profile): a new directory, whose parent must exist, or an existing empty one, such as a mounted volume. Anything else in it is refused (`output-exists`, 4). A directory `import` created is removed if it fails; an existing one is left empty.
- **`chain.json` marks the directory as a chain.** A command treats its input as a chain when it is a directory holding `chain.json`. Its `format` is the chain format's version, and `source` the source file's name. `import` publishes it after stage 000, so a directory is a chain only once it has a stage. A directory without it, given to a command that reads chains, is rejected (`not-a-chain`, 1).
- **A stage is a directory `NNN-<command>-<id>`**, numbered from 000 in decimal (three digits, more once past 999; they are compared as numbers). `<id>` is five random characters from `a-z0-9`. Today the lock makes numbers unique already; the id keeps names unique if a later feature runs a step again at a position that has a stage, and it makes the name the stage's identity, which `parent` refers to. A command name can contain hyphens (`simplify-qem`): the number is the part before the first hyphen, the id the part after the last one. A stage holds the model in the normalized profile ([normalized-profile.md](normalized-profile.md)) and `stage.json`. The model's stem is the source file's stem in every stage, so file names stay the same from stage to stage, and messages show the source's name.
- **The last stage** is the one with the highest number. A directory that doesn't match `NNN-<command>-<id>` or has no `stage.json`, such as a killed run's `NNN-<command>-<id>.meshtinker-<random>.tmp`, is ignored. Which stage is last when two share a number is for the feature that makes it possible to decide; until then the lock prevents it, and readers take the last name in byte order.
- **Every stage is kept.** A stage is never changed once published, and nothing removes stages except `export --remove-chain`, which removes the whole chain. A meta-command's chain lives in its workspace and goes with it.

## No log file: the stages are the log

Each stage is written into a temporary directory beside its final name, then renamed into place without replacing anything (`renameat2(RENAME_NOREPLACE)` on Linux, `renamex_np(RENAME_EXCL)` on macOS), the way `common::Outputs` publishes files ([files-and-memory.md](files-and-memory.md#outputs)). That rename publishes the stage. A separate log file, appended to after the rename, could disagree with the directories after a `SIGKILL` between the two. With the record inside the stage, a stage exists with its record or not at all. The ordered stages and their `stage.json` are the log, and a command that prints them (`export --log`, or a `chain` command) can come later.

This is also why the record is a plain JSON file and not a line appended to a text, JSON Lines or SQLite log. Nothing is ever appended. SQLite would also add a dependency, and `stage.json` can be read with `cat` and `jq`.

## `stage.json`

```json
{
  "format": 1,
  "stage": "001-simplify-7xd2m",
  "parent": "000-import-k3v9q",
  "command": "simplify",
  "tool": "meshtinker-simplify 0.3.0",
  "argv": ["meshtinker-simplify", "--error", "0.001", "work/"],
  "report": { "...": "the command's --json output" },
  "lossy": {
    "geometryQuantization": [{"stage": "000-import-k3v9q", "source": "KHR_draco_mesh_compression", "primitives": 4}],
    "simplificationError": 0.00098,
    "imageEncoding": [{"stage": "000-import-k3v9q", "image": 0, "format": "image/jpeg"}]
  }
}
```

- **`stage` and `parent`** are directory names: the stage's own, and the one it was built from, always the previous stage for now (`null` for stage 000). Building a stage from an older one (`--from`, to compare two variants) is out of scope, but readers follow `parent` rather than assume the previous number, so adding it changes no format.
- **`report`** is the command's JSON report. `import`'s is the `inspect` report ([inspect-report.md](inspect-report.md)), so stage 000 has the model's statistics without another pass. Later stages carry their own command's report, and a caller that needs fresh statistics runs `inspect` on the chain.
- **`tool` and `argv`** make a chain reproducible by hand. A variant built from another branch writes stages too: readers ignore fields they don't know, and refuse a `format` newer than theirs.
- **`lossy`** is the running total of lossy rounds, copied from the parent and extended by the stage. The file alone can't tell it once a stage has decoded Draco to float32 or re-encoded a JPEG. Errors add up ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)), so a command about to add a round warns, with a counted diagnostic, when an earlier stage recorded one of the same kind: `repeated-quantization`, `repeated-lossy-image-encoding`. The warning is in the stage's report too. Rounds of one stage don't warn about each other. The rounds recorded today:
  - `import` records what the source went through, as far as the file tells: `geometryQuantization` entries for Draco primitives (`source`, `primitives`) and for `KHR_mesh_quantization`, and an `imageEncoding` entry (`image`, `format`) for each JPEG, WebP and KTX2 image. A WebP image may be lossless and is recorded anyway; Draco's bit depths aren't read.
  - `compress` records one `geometryQuantization` round when it compresses primitives: `source: "EXT_meshopt_compression"`, `primitives`, its preset's `positionBits` (when a mesh went on the grid), `normalBits` and `texcoordBits`.
  - `simplify` adds to `simplificationError` the largest error meshoptimizer reported (its `achievedError`), not the `--error` asked for, relative to the scene's size: the sum bounds how far the surface moved over the chain, as far as the estimate goes.

  File mode has no history, and recording it in the model (`asset.extras`) was rejected for the same reasons as an import marker ([normalized-profile.md](normalized-profile.md#how-commands-get-normalized-input)).

## Command kinds

`cli::Command` declares the `input [output]` positionals once for each kind and resolves them to an input file, an output and, in chain mode, the chain. Subcommands don't handle chains themselves.

| Kind | Commands | File mode | Chain mode |
| --- | --- | --- | --- |
| constructor | `import` | `import in out.gltf` | `import in work/` creates the chain and stage 000 |
| transform | normals, weld, dedup, simplify, compress, uv, ktx, avif | `cmd in out.gltf` | `cmd work/` adds a stage |
| read-only | `inspect` | `inspect in` | `inspect work/` reads the last stage |
| terminal | `export` | — | `export work/ out.glb` |

- `placeholder` stays out of chains and reads files only. Commands that read a model and write something other than its next version may not stay, so chains aren't designed around them.
- `optimize` is a files command too: it builds a chain of its own in its workspace, which the user never names. Its output may be `.glb` as well as `.gltf`, which a files command declares (`cli::OutputFormats`), since the kind is about chains, not formats.
- Only `import` creates a chain. A transform given a chain and an output is a command-line error (2): a stage's location is the chain's to decide. So is a transform given a file and no output, and `export` given anything but a chain.
- A command declares its kind when it creates its `cli::Command`, which resolves the positionals before the command's body runs: in chain mode `input()` is the last stage's model, `outputDirectory()` the pending stage directory, `outputStem()` the source's stem, and `stage()` the stage being written, for `lossy`. The body writes through `outputs()` and calls `publish(report)`, which writes `stage.json` and publishes the stage. A transform therefore needs no code of its own for chains beyond its `lossy` rounds.
- In chain mode a command's input is always `import`'s output or a stage built on it, so it is in the profile and read front to back.
- The workspace (`--tmp-dir`) defaults to the chain directory.
- `export` writes the last stage as `.gltf` (with its `.bin` and images) or as `.glb`, depending on the output's extension. A GLB streams the images into its BIN chunk. It is the one command that writes outside the profile. `--remove-chain` removes the chain once the output is published. It takes the lock first and reads the last stage under it, so that no stage is added and lost meanwhile, and the output's files are then hard links to the stage's, since the chain goes. It removes `chain.json` first, so that an interrupted removal leaves no half chain, then everything else in the directory and the directory itself. The directory belongs to the chain: files the user put there go too, and an output inside it is refused (exit code 2) rather than deleted with it. A directory that can't be removed, such as a mount point `import` was given empty, stays empty, with a warning (`chain-directory-kept`).

## Unchanged files are linked, not copied

A texture-only stage must not copy a multi-gigabyte `.bin`, and a geometry stage must not copy the textures. When a stage writes a file identical to its parent's (an image it doesn't touch, or a `.bin` whose views it copies unchanged, in order), it hardlinks the parent's file, and copies it only if the link fails. Stages are never changed once published and inputs are mapped read-only, so sharing an inode is safe. Each stage stays self-contained: its files are in its own directory, so the [reference rules](../AGENTS.md#command-line-conventions) hold without `--trusted`. Referencing `../000-import/foo_texture_0.png` instead would break them.

`import` copies from outside the chain, with a reflink (`FICLONE`, then `copy_file_range`) where the file system has one. It never hardlinks the source: an editor that rewrites the source in place would change the chain. File-mode commands and `export` copy the same way. How the writer decides that a file is identical is in [gltf-io.md](gltf-io.md#writing).

## Concurrency

Two transforms on one chain would both build on the same stage. A command that writes a stage holds `flock` on `work/lock` from reading the last stage to publishing its own, and fails at once with exit code 4 (`chain-in-use`) if another command holds it. The kernel releases the lock when a process dies, so no stale lock is left. Read-only commands take no lock: they read a published stage, which never changes.

## Meta-commands

`optimize` ([optimize.md](optimize.md)) is a C++ subcommand (`meshtinker-optimize`) that runs the individual subcommands, as AGENTS.md says. Scripts would need a runtime (a shell, Node.js) beside the static binaries. It decides from the JSON reports, which nlohmann reads, and a user who wants to experiment can script a chain by hand.

1. Create a chain in its workspace, beside the output, so that failure and interruption cleanup come with the workspace, and `--keep-tmp` keeps the whole chain for debugging.
2. Run `import in chain/`. Stage 000's report has the statistics.
3. Run the plan (`src/commands/optimize/plan.cpp`): a list of conditions and steps, which reads the preset (`fast`, `slow`), the quality (`Highest` to `Thumb`), the reports so far and the memory available, and runs each step on the chain with `--json` as it goes.
4. Run `export chain/ out.glb`.

Steps run in this order; a plan may leave any of them out.

| Step | Must come after | Why |
| --- | --- | --- |
| `import` | | Every other command reads a chain's stages. |
| `normals` | `import` | Writes smooth normals where they're missing or all flat (AGENTS.md, "normals default to smooth"). Flat normals keep the vertices of a flat soup apart, so it must come before `weld`. |
| `weld` | `normals` | Merges vertices equal in every attribute. It must come before `simplify`, which collapses only edges between shared vertices. |
| `dedup` | `weld` | Stores identical images, materials, meshes and accessors once. After `weld`, which copies primitives that share accessors; before `simplify`, which locks copies of a mesh whole, and before the image commands, so that each image is baked and encoded once ([dedup.md](dedup.md#in-a-chain)). |
| `simplify` | `dedup` | Fewer triangles make every later step cheaper. |
| `uv` | `simplify` | It repacks the texture coordinates that `simplify` keeps, on fewer triangles. It must come before `ktx` and `avif`: it decodes neither KTX2 nor AVIF ([uv.md](uv.md)). |
| `ktx` or `avif` | `uv` | Encode the images that `uv` baked: KTX2, which stays compressed on the GPU, or AVIF, smaller to download. One or the other: each leaves the other's textures as they are, since `ktx` can't decode AVIF and `avif` keeps KTX2 ([avif.md](avif.md#textures-and-sources)). |
| `compress` | every geometry step | It quantizes and encodes geometry. A geometry step after it would write its primitives back as float32 ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)). |
| `export` | | Writes the last stage. |

`compress` stays one command, rather than a `quantize` and a `compress`. Vertices that become identical once quantized would merge inside `compress`, with the remap `weld` uses. A `weld` stage between the two would otherwise have to keep quantized storage, unlike every other geometry command, and `compress` would have to accept input it didn't quantize.

- **Finding subcommands** uses the wrapper's lookup (own directory, then `PATH`), in `common` (`common/subcommands.hpp`).
- **Output.** Children's stderr passes through, so progress and warnings appear once, from the command that raised them. With `--json`, `optimize` prints `steps: [{command, argv, exitCode, seconds, report}]` and merges the children's diagnostics, each tagged with its step.
- **Exit codes.** A child's non-zero exit code becomes `optimize`'s. A child killed by a signal is an internal error (70). In a container a `SIGKILL` most likely comes from the cgroup's OOM killer, but nothing confirms it, so it is not reported as out of memory (3).
- **Interruption.** A container's `SIGTERM` reaches only `optimize`. It forwards `SIGINT`, `SIGTERM` and `SIGHUP` to the running child (`common::deferInterruptions`), waits for it, unwinds to remove its workspace, then dies of the signal.
