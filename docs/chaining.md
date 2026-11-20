# Chaining commands

Commands can be run one after another on a *chain*: a directory that holds each step's result as a numbered *stage*. A user chains commands by hand with no file names to wire, and meta-commands such as `optimize` chain them the same way. This is the design. TODO.md tracks its implementation, and the README will document the commands once they support it.

```bash
meshtinker import foo.glb work/          # output is a directory: creates the chain, stage 000
meshtinker simplify work/                # input is a chain, no output: adds a stage built on the last one
meshtinker compress work/
meshtinker inspect work/                 # reads the last stage
meshtinker placeholder work/ ph.gltf     # reads the last stage, writes outside the chain
meshtinker export work/ out.glb          # writes the last stage as GLB or .gltf
```

File mode stays as it is: `meshtinker simplify in.gltf out.gltf` reads and writes plain files, with no chain. eCorpus and experimental variants can keep using it.

## Layout

```
work/
  chain.json                 {format, source: "foo.glb", created}
  lock                       held with flock while a command writes a stage
  000-import/   foo.gltf foo.bin foo_texture_0.png  stage.json
  001-simplify/ foo.gltf foo.bin foo_texture_0.png  stage.json
  002-compress/ ...
```

- **The chain is a directory the user names.** It is not looked up from the input file's path in a cache directory, which would break on two pipelines over one file, relative paths and symlinks, and would hide state that nothing cleans. It is not created beside the source either, since source folders may be read-only or must not be touched. It goes where the user puts it, on a real disk ([files-and-memory.md](files-and-memory.md#workspaces)). `import` always takes the chain directory as its output: there is no default location.
- **`chain.json` marks the directory as a chain.** A command treats its input as a chain when it is a directory holding `chain.json`. Its `format` is the chain format's version.
- **A stage is a directory `NNN-<command>`**, numbered from 000 in decimal (three digits, more once past 999; they are compared as numbers). It holds the model in the normalized profile ([normalized-profile.md](normalized-profile.md)) and `stage.json`. The model's stem is the source file's stem in every stage, so file names stay the same from stage to stage, and messages show the source's name.
- **The last stage** is the one with the highest number. A directory that doesn't match `NNN-<name>` or has no `stage.json`, such as a killed run's `NNN-<name>.meshtinker-<random>.tmp`, is ignored.
- **Every stage is kept.** A stage is never changed once published, and nothing removes stages except `export --remove-chain`, which removes the whole chain. A meta-command's chain lives in its workspace and goes with it.

## No log file: the stages are the log

Each stage is written into a temporary directory beside its final name, then renamed into place without replacing anything (`renameat2(RENAME_NOREPLACE)` on Linux, `renamex_np(RENAME_EXCL)` on macOS), the way `common::Outputs` publishes files ([files-and-memory.md](files-and-memory.md#outputs)). That rename publishes the stage. A separate log file, appended to after the rename, could disagree with the directories after a `SIGKILL` between the two. With the record inside the stage, a stage exists with its record or not at all. The ordered stages and their `stage.json` are the log, and a command that prints them (`export --log`, or a `chain` command) can come later.

This is also why the record is a plain JSON file and not a line appended to a text, JSON Lines or SQLite log. Nothing is ever appended. SQLite would also add a dependency, and `stage.json` can be read with `cat` and `jq`.

## `stage.json`

```json
{
  "format": 1,
  "stage": 1,
  "parent": 0,
  "command": "simplify",
  "tool": "meshtinker-simplify 0.3.0",
  "argv": ["meshtinker-simplify", "--error", "0.001", "work/"],
  "report": { "...": "the command's --json output" },
  "lossy": {
    "geometryQuantization": [{"stage": 0, "source": "KHR_draco_mesh_compression", "positionBits": 14}],
    "simplificationError": 0.001,
    "imageEncoding": [{"stage": 0, "image": 0, "format": "image/jpeg"}]
  }
}
```

- **`parent`** is the stage this one was built from, always the previous one for now. Building a stage from an older one (`--from N`, to compare two variants) is out of scope, but readers follow `parent` rather than assume `stage - 1`, so adding it changes no format.
- **`report`** is the command's JSON report. `import`'s is the `inspect` report ([inspect-report.md](inspect-report.md)), so stage 000 has the model's statistics without another pass. Later stages carry their own command's report, and a caller that needs fresh statistics runs `inspect` on the chain.
- **`tool` and `argv`** make a chain reproducible by hand. A variant built from another branch writes stages too: readers ignore fields they don't know, and refuse a `format` newer than theirs.
- **`lossy`** is the running total of lossy rounds, copied from the parent and extended by the stage. The file alone can't tell it once a stage has decoded Draco to float32 or re-encoded a JPEG. Errors add up ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)), so a command about to add a round warns, with a counted diagnostic, when one of the same kind is already there: `repeated-quantization`, `repeated-lossy-image-encoding`. A simplification adds its error to `simplificationError`. File mode has no history, and recording it in the model (`asset.extras`) was rejected for the same reasons as an import marker ([normalized-profile.md](normalized-profile.md#how-commands-get-normalized-input)).

## Command kinds

`cli::Command` declares the `input [output]` positionals once for each kind and resolves them to an input file, an output and, in chain mode, the chain. Subcommands don't handle chains themselves.

| Kind | Commands | File mode | Chain mode |
| --- | --- | --- | --- |
| constructor | `import` | `import in out.gltf` | `import in work/` creates the chain and stage 000 |
| transform | simplify, compress, uv, ktx | `cmd in out.gltf` | `cmd work/` adds a stage |
| read-only | `inspect` | `inspect in` | `inspect work/` reads the last stage |
| derive | `placeholder` | `cmd in out.gltf` | `cmd work/ out.gltf` reads the last stage, adds none |
| terminal | `export` | — | `export work/ out.glb` |

- Only `import` creates a chain. A transform given a chain and an output is a command-line error (2): a stage's location is the chain's to decide.
- In chain mode a command's input is always `import`'s output or a stage built on it, so it is in the profile and read front to back.
- The workspace (`--tmp-dir`) defaults to the chain directory.
- `export` writes the last stage as `.gltf` (with its `.bin` and images) or as `.glb`, depending on the output's extension. A GLB streams the images into its BIN chunk. It is the one command that writes outside the profile. `--remove-chain` removes the chain once the output is published.

## Unchanged files are linked, not copied

A texture-only stage must not copy a multi-gigabyte `.bin`, and a geometry stage must not copy the textures. When a stage writes a file identical to its parent's (an image it doesn't touch, or a `.bin` whose views it copies unchanged, in order), it hardlinks the parent's file, and copies it only if the link fails. Stages are never changed once published and inputs are mapped read-only, so sharing an inode is safe. Each stage stays self-contained: its files are in its own directory, so the [reference rules](../AGENTS.md#command-line-conventions) hold without `--trusted`. Referencing `../000-import/foo_texture_0.png` instead would break them.

`import` copies from outside the chain, with a reflink (`FICLONE`, then `copy_file_range`) where the file system has one. It never hardlinks the source: an editor that rewrites the source in place would change the chain.

## Concurrency

Two transforms on one chain would both build on the same stage. A command that writes a stage holds `flock` on `work/lock` from reading the last stage to publishing its own, and fails at once with exit code 4 (`chain-in-use`) if another command holds it. The kernel releases the lock when a process dies, so no stale lock is left. Read-only and derive commands take no lock: they read a published stage, which never changes.

## Meta-commands

`optimize` and other presets are C++ subcommands (`meshtinker-optimize`) that run the individual subcommands, as AGENTS.md says. Scripts would need a runtime (a shell, Node.js) beside the static binaries. Presets decide from the JSON reports, which nlohmann reads, and a user who wants to experiment can script a chain by hand.

1. Create a chain in its workspace, beside the output, so that failure and interruption cleanup come with the workspace, and `--keep-tmp` keeps the whole chain for debugging.
2. Run `import in chain/`. Stage 000's report has the statistics.
3. Build a plan from the preset (`fast`, `slow`), the quality (`Highest` to `Thumb`) and the statistics: which steps run, their options, and which tool each uses for the model's size and the memory available. The plan is a table in code, like eCorpus's `getPreset` (`optimizeGlb.ts`).
4. Run each step on the chain with `--json`, then `export chain/ out.glb`.

- **Finding subcommands** uses the wrapper's lookup (own directory, then `PATH`), moved into `common`.
- **Output.** Children's stderr passes through, so progress and warnings appear once, from the command that raised them. With `--json`, `optimize` prints `steps: [{command, argv, exitCode, report}]` and merges the children's diagnostics, each tagged with its step.
- **Exit codes.** A child's non-zero exit code becomes `optimize`'s.
- **Interruption.** A container's `SIGTERM` reaches only `optimize`. It forwards `SIGINT`, `SIGTERM` and `SIGHUP` to the running child, waits for it, then cleans its workspace. The current handler only unlinks and re-raises, so spawning needs a variant of it.
