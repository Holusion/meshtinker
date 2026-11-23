# Files, memory and failures

How commands use memory and disk, publish outputs and report failures. The code is in `include/meshtinker/common/`. Much of it follows mesh2gltf (`disk.rs`, `failure.rs`, `references.rs`).

## Memory

- **Inputs are mapped** (`MappedFile`): read-only, `MAP_PRIVATE`, whole files. Their pages are page cache: they count in RSS while memory is free, and are dropped rather than swapped under pressure. `releasePages()` drops pages already read, for a pass that won't come back.
- **Scratch space is mapped too** (`Workspace::scratch`): a zero-filled file of the requested size, mapped read-write and shared. `posix_fallocate` reserves its blocks first, so a full disk fails with exit code 3 when the file is created, not as a `SIGBUS` on first write. File systems without `fallocate` get a sparse file instead.
- **Working arrays** (`common::LargeArray`), such as a primitive's positions and indices while a command rewrites it, go on the heap while all of them hold 256 MiB together, and in scratch blocks beyond: 64 MiB scratch files shared by many arrays, or a file of its own for an array over 16 MiB (`Workspace::block`). A shared file's space comes back when its last array goes. meshoptimizer's allocations share the budget (`gltf::MeshoptMemory`). The budget is a constant, per process: servers run several commands at once, and each spills its large models rather than holding them on the heap, where the kernel can't reclaim them. 256 MiB fits 93 % of the local collection, and costs spilled runs a few percent of time; measured with other budgets in [experiments/heap-budget-results.md](experiments/heap-budget-results.md). A per-array limit wasn't enough: a scan in 634 small primitives held each under it, and the whole model on the heap. For measurements, `-DMESHTINKER_HEAP_ARRAY_LIMIT=<bytes>ULL` sets the budget at compile time, and `-DMESHTINKER_ARRAY_STATS` writes the most it held to the file `MESHTINKER_ARRAY_STATS_FILE` names; [experiments/heap-budget-results.md](experiments/heap-budget-results.md) has what they measured.
- The heap holds the JSON (twice, see [gltf-io.md](gltf-io.md)) and per-object records. `tools/monitor.py` reports `peak_anon_kib` separately from file-backed RSS: that figure is the one that must stay small.

## Workspaces

A `Workspace` is a private directory, `meshtinker-<pid>-<random>`. The random part keeps a killed run's leftovers from colliding with the next run in another PID namespace.

- It goes in the output's directory, or the input's for read-only commands. The OS temporary directory is often RAM-backed, which would turn scratch files back into memory. `--tmp-dir` picks another parent.
- It is created on first use, so a command that needs no scratch space writes nothing.
- Scratch files are unlinked as soon as they are mapped, so their disk space returns even if the process is killed.
- The directory is removed when the command ends or is interrupted. With `--keep-tmp`, nothing is unlinked or removed, and the path is printed to stderr.
- If the directory can't be created, the command fails with exit code 4 and suggests `--tmp-dir`.

## Outputs

`Outputs` publishes files only when the command succeeds:

1. `create(path)` fails with `output-exists` (exit 4) if `path` exists, so a command never does its work only to find it can't write.
2. Otherwise it writes a temporary file beside it, `<name>.meshtinker-<random>.tmp`. It is not hidden, so leftovers of a killed run show up next to the outputs, and `*.meshtinker-*.tmp` matches them and nothing else, for cleanup scripts. A missing destination directory fails here (exit 4).
3. `commit()` moves each temporary file to its destination without ever replacing a file. It uses `renameat2(RENAME_NOREPLACE)` on Linux, a hard link elsewhere, and on file systems with neither, a check followed by a rename.
4. If the command fails before or during `commit()`, every temporary file and every file already published by this run are removed.

Readers watching the directory never see a partial output under its final name.

A directory is published the same way, whole (`createDirectory`), as a chain's stages are ([chaining.md](chaining.md)). It is created as `<name>.meshtinker-<random>.tmp`, its files are written inside it under their final names, and `commit()` renames it with `renameat2(RENAME_NOREPLACE)` on Linux, `renamex_np(RENAME_EXCL)` on macOS, and elsewhere by claiming the name with `mkdir` and renaming over that empty directory. A failed run removes it with its files.

`copy(source, bytes, destination)` writes a file identical to a whole input file without reading it through the process: a hard link when the source is in the directory given to `linkFrom` (a chain's parent stage, whose files never change), else a reflink (`FICLONE`), else `copy_file_range`, else a plain write of the mapped bytes. A source whose size no longer matches its mapping gets the mapped bytes, which are what the command read. Other files are never hard linked: an editor that rewrites a source in place would change the output too.

`common::FileLock` takes an exclusive `flock` without waiting, which chains use to keep two commands from writing a stage at once. The kernel releases it when the process dies. Write errors are classified like any system error: a full disk, quota or file-size limit gives exit code 3, anything else 4.

## Interruption

`installInterruptCleanup()` handles `SIGINT`, `SIGTERM` and `SIGHUP`. Temporary outputs and workspace directories register themselves (`CleanupRegistration`). On a signal, the handler removes them with `unlink` and `rmdir`, which are safe in a signal handler: files first, then directories, one pass per nesting level (a chain directory, a stage in it), then lets the signal kill the process as usual. `SIGKILL` can't be caught: it leaves the `<name>.meshtinker-<random>.tmp` files beside the outputs and the empty `meshtinker-<pid>-<random>` workspace directory, never a partial file under its final name.

## Failures and exit codes

Expected failures throw `common::Failure`, which carries an exit code and a stable `code` for JSON consumers. `common::describeFailure` turns any exception that escapes a command into a report:

| Exception | Exit code | `code` |
| --- | --- | --- |
| `Failure` | its own | its own |
| `std::bad_alloc` | 3 | `out-of-memory` |
| `std::filesystem::filesystem_error` | 3 or 4, by its `errno` | `file-system` |
| anything else | 70 | `internal` |

`classifySystemError` decides the class of an `errno` value:
- `ENOSPC`, `EFBIG` and `EDQUOT` are resource limits (3).
- `ENOENT`, `EISDIR`, `ENOTDIR`, `ENAMETOOLONG` and `ELOOP` reject the input (1) when reading the input or a file it references, and are machine problems (4) elsewhere. A missing input is one exception: `export` reads chains only, which an earlier command made, so a chain that isn't there is the caller's problem (4), not the model's.
- Everything else is a machine problem (4).

Messages name files, never their folders.
