# Experiment: the working-array heap budget, and punching holes in scratch files

A handoff for an unattended run. It must make sense from the repository alone. Work on the `heap-budget-experiment` branch, in its own worktree, and push it when done. Don't merge it, don't push to `main`, don't touch other branches.

## Questions

1. **What heap budget should working arrays have?** Today it's 4 MiB, a constant. It must stay a constant: on servers with more memory, commands are expected to run in parallel, which is where spilling to scratch files helps most. A budget sized from the memory available was considered and rejected for that reason.
2. **Would punching holes in scratch files help, by how much, and in which cases?** That means dropping the pages of a freed block instead of letting the kernel write them back.

## What the code does today

The code is on this branch, which starts from `meshopt` at `4cc24e5`.

- `common::LargeArray` (`include/meshtinker/common/large_array.hpp`) holds per-vertex and per-face working data. It goes on the heap while all LargeArrays together hold at most `heap_array_limit` (4 MiB), and in a scratch block beyond that.
- `gltf::MeshoptMemory` routes meshoptimizer's allocations through the same budget.
- `Workspace::block` (`src/common/workspace.cpp`) hands out scratch blocks: 64 MiB scratch files shared by many blocks, bump-allocated, or a file of its own for a block over 16 MiB. A shared file is unmapped when its last block is released (a `shared_ptr` per file). Files are unlinked as soon as they are mapped, and their descriptor is closed. Pages of an unlinked file are dropped without writeback once it's unmapped. But while it's mapped, the kernel writes its dirty pages back on its own schedule.
- `docs/files-and-memory.md` describes all of this. `docs/simplify.md` and `docs/compress.md` describe the two commands that use it.

## What is known

These figures are from the field notes in `docs/validation.md`. Release builds, inputs in the page cache, two runs each.

| Run | Output disk | 4 MiB | 256 MiB | Unlimited | Heap, unlimited |
| --- | --- | --- | --- | --- | --- |
| 44M simplify (`Zodiac/MAN63675_HP_44M`) | SSD | 61.6 s | 58.9 s | 53.1 s | 2.8 GB |
| same | HDD | 76.4 s | 72.4 s | 53.2 s | |
| 44M compress | either | 18.1 s | 17.3 s | 16.0 s | 1.3 GB |
| Notre Dame simplify (10M faces, 30 primitives) | either | 10.7 s | 10.5 s | 9.8 s | 445 MB |

- **Disk writes.** At 4 MiB the 44M simplify wrote about 5 GB to the output's disk during the run. That's more than its working set, because pages get rewritten. With no budget it wrote only the output (about 230 MB).
- **Memory.** Peak RSS is about 4 GB in every case. Mapped scratch doesn't use less memory while RAM is free; it only lets the kernel reclaim it under pressure.
- **Writeback even on short runs.** A sample during a 10 s Notre Dame simplify at 4 MiB (`tools/experiments/dirty-probe.py`) showed dirty memory climbing to 1.4 GB, and writes starting at 6 s, at about 880 MB dirty. That's below the machine-wide background threshold (`nr_dirty_background_threshold`, 1.4 GB here), so the trigger is probably the disk's share of that threshold. Past `nr_dirty_threshold` (2.9 GB here), the kernel pauses the process at the disk's write speed. That's why the spinning disk is slower.
- **Why punching holes might help.** A shared 64 MiB file keeps the dirty pages of blocks already freed until its last block goes. Those pages count toward the thresholds and can get written for nothing. `simplify` frees each space's input arrays when it moves on, but its outputs stay live until the write, so freed and live blocks get interleaved in the same files.

- **The shared-position search in `simplify`** (`sharedPositions` in `src/commands/simplify/simplify.cpp`) writes one 16-byte entry per vertex of the scene: 400 MB for the 44M scan, in a scratch file of its own past the budget. It sorts the entries in place, which dirties every page, and frees the file after about 3 s. Pages not written by then are dropped, but on large models writeback is usually already running. Measure how much of the writes it accounts for: `dirty-probe.py` around that phase, or a variant that skips the search. And say whether a smaller entry (a 64-bit hash plus the primitive, checked against the positions on a match) is worth it.

## The machine

- 4 cores and 31 GB of RAM.
- The repository and `build/` are on an SSD (`/dev/sdd`, btrfs with zstd).
- `/home/sebastien/Documents` is a **spinning disk** (`/dev/sda`, btrfs with zstd). The models are there.
- Keep the machine otherwise idle while timing.

## Models

The models are read in place, **never modified**. Write nothing inside `/home/sebastien/Documents/3DModels`.

- **All of `/home/sebastien/Documents/3DModels/collection/output/*/model.gltf` (56 models).** All plain geometry, median 200,000 faces, largest 2.3M, 4 with several primitives. This is the bulk of what eCorpus processes. Use them for the threshold question: which share of models fits entirely under each budget, and what spilling costs on them.
- **Larger models from `/home/sebastien/Documents/3DModels`, for the tail:**

  | Path | Layout |
  | --- | --- |
  | `Zodiac/MAN63675_HP_44M/MAN63675_HP_44M.gltf` | 44M faces, 4 meshes in one space |
  | `Zodiac/Tiles/model.gltf` | 44M faces, 1 mesh in 51 primitives |
  | `Notre Dame en Vaux/Notre_Dame_en_Vaux_10M.gltf` | 9.9M faces, 30 primitives |
  | `Lascaux/output/source_model_8k/Lascaux_seamed_8k.gltf` | 4.7M faces, 166 primitives |
  | `ISTI_dataset/RWT180/WoodenDragonSculpture.gltf` | 3.1M faces, 1 primitive: meshoptimizer's largest single allocations |
  | `Lion_Belfort.Sc.130.3/test_draco.glb` | Draco: the decoder allocates on its own |

  Add others only if they add a data point the list lacks. Check their layout first with `build/meshtinker-inspect --json`.

## Setup in the worktree

```sh
git worktree add ../meshtinker-heap-budget heap-budget-experiment
cd ../meshtinker-heap-budget
cp ../meshtinker/test/corpus.local.json test/   # gitignored, needed by tools/corpus.mjs
npm ci --prefix tools                            # only if tools/node_modules is needed (validator, compare)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
```

**Variants.** Make `heap_array_limit` overridable at compile time:

```cpp
#ifdef MESHTINKER_HEAP_ARRAY_LIMIT
inline constexpr std::uint64_t heap_array_limit = MESHTINKER_HEAP_ARRAY_LIMIT;
#else
inline constexpr std::uint64_t heap_array_limit = std::uint64_t{4} << 20;
#endif
```

Build one directory per variant, `build/budget-<name>`, with `-DBUILD_TESTING=OFF "-DCMAKE_CXX_FLAGS=-DMESHTINKER_HEAP_ARRAY_LIMIT=<bytes>ULL"`, targets `meshtinker-simplify` and `meshtinker-compress`. Budgets: 4 MiB, 16 MiB, 64 MiB, 256 MiB, 1 GiB, and unlimited (`18446744073709551615ULL`). Commit the `#ifdef` if it proves useful; it changes nothing by default.

**Punch-hole variant.** When a block inside a shared scratch file is released, call `madvise(MADV_REMOVE)` on the whole pages it covers. The file's descriptor is closed, so `fallocate` isn't available. `MADV_REMOVE` frees both the pages and the file's backing store, and btrfs supports it. `MADV_DONTNEED` would not do: on a shared file mapping it only unmaps the pages, which stay dirty in the page cache. Blocks are 16-byte aligned, so round inward to page boundaries. That needs a per-block release hook: `ScratchBlock` holds the file's `shared_ptr` and its pointer, so give it a size and a destructor. Files of their own (blocks over 16 MiB) need nothing, since unmapping an unlinked file already drops its pages. Build it as `build/budget-<name>-punch` for at least 4 MiB and 64 MiB, and for the budget you end up recommending.

## Running

`tools/experiments/heap-budget.sh RESULTS MODELS "VARIANTS" "COMMANDS" "DISKS" REPEATS` runs the matrix and writes one JSON line per run. It reads the inputs into the page cache first, runs `sync` before each run, and records the sectors written to the output's disk, both during the run and after a final `sync`.

- Outputs and the workspace go to `build/heap-budget/out` (SSD) or `/home/sebastien/Documents/meshtinker-bench/out` (HDD). Delete the HDD folder when you're done.
- It skips runs whose result already exists, so it can resume.

`tools/experiments/dirty-probe.py DEVICE -- COMMAND...` samples dirty memory and disk writes during one run. Use it to see when writeback starts, for instance on one collection model and one large one, with and without holes.

Suggested plan, which keeps the whole night under about 5 hours:

1. **Collection, SSD only, one repeat:** every budget, both commands. Small runs are fast, but 56 models × 6 budgets × 2 commands is 672 runs. If it takes too long, keep 4 MiB, 64 MiB and unlimited first.
2. **Large models, SSD and HDD, two repeats:** 4 MiB, 64 MiB, 256 MiB, 1 GiB and unlimited, plus the punch-hole variants. The two 44M models take about a minute per simplify run, so start with them and watch the clock.
3. **`dirty-probe.py`** on a few runs where the punch-hole variant differs most.

`monitor.py` refuses to overwrite a run, and says so on stderr. Never hide its stderr, or you'll read the previous run's figures without noticing. Give each run a new name; the script does.

## What to deliver

Commit each of these on this branch, and push the branch:

1. **`docs/experiments/heap-budget-results.md`**, with:
   - for each budget, the share of collection models whose working set fits under it, and the time and disk writes against unlimited;
   - the large models' table (time, heap peak, peak RSS, MiB written during the run and in total), SSD and HDD;
   - the punch-hole variant against its plain budget: how many MiB of writes it saves, and the time it gains or loses, by model kind (many small primitives, one big primitive, several meshes, Draco);
   - a recommendation for `heap_array_limit`, and for or against punching holes. Give reasons tied to parallel runs on servers: the budget is per process, and N processes may run at once.
2. **If punching holes helps**, a clean implementation: a test in `test/common/workspace_test.cpp` (writing to a freed block's pages doesn't fail; a later block still works), and a paragraph in `docs/files-and-memory.md`. Run the full pre-commit checks from AGENTS.md: release and sanitizer builds with ctest, plus `cmake --build build --target tidy`.
3. **Rows in the field notes** at the end of `docs/validation.md`, in the existing table's style.
4. **A rewrite of the "flexible memory management" section of `TODO.md`** with the findings, if that file exists in the worktree. It's gitignored here, so it won't; in that case, write the suggested text at the end of the results document instead.

If something blocks the run (a model fails, the disk fills, a build breaks), record it in the results document and carry on with what still works. Don't install system packages.
