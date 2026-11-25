# Results: the working-array heap budget, and punching holes in scratch files

The answers to [heap-budget.md](heap-budget.md), from an unattended run on 21 November 2026, 02:03 to 05:30, on an otherwise idle machine (4 cores, 31 GB; SSD `/dev/sdd` and HDD `/dev/sda`, both btrfs with zstd). Release builds, inputs in the page cache, outputs and workspaces on the disk named. Raw figures: one JSON line per run, from `tools/experiments/heap-budget.sh`; they were in `build/heap-budget/` of the worktree and aren't committed, the tables below hold them.

## Recommendation

- **`heap_array_limit`: 256 MiB**, still a constant.
- **Don't punch holes in scratch files.** On btrfs it makes things worse in every case measured.
- **Keep the shared-position search as it is.** A smaller entry isn't worth its code.

### Why 256 MiB

- **The collection fits.** 52 of the 56 collection models (93 %) simplify without a scratch file at 256 MiB, and 54 (96 %) compress without one; at 4 MiB, 8 and 10 do. That is the bulk of what eCorpus processes.
- **What spilling costs on them is time, not disk.** On the collection no budget writes anything during the run: the scratch files are unlinked and unmapped before writeback starts, so their pages are dropped. What a small budget costs is the page faults and file setup of mapped scratch: +16 % on `simplify` at 4 MiB and +13 % at 64 MiB against unlimited, +3 % at 256 MiB.
- **Large models gain from it too, a little.** 256 MiB takes the 44M scans from 59.0 to 54.7 s (4 meshes) and 44.2 to 38.4 s (51 primitives) on the SSD, and scan-10m from 12.5 to 9.8 s. Their writes barely change between 64 MiB and 1 GiB: 2 GB of a 2.8 GB or 1.8 GB working set gets written either way, and only no budget at all avoids it.
- **It stays small next to parallel runs.** The budget is per process. Four processes on a 4-core server, one per core, keep at most 1 GiB of working arrays on the heap together, eight keep 2 GiB, whatever the models. Past that, large models spill, which is what lets several of them run at once on a server that couldn't hold all their working sets: the kernel can reclaim mapped scratch pages under pressure, and can't reclaim the heap. 1 GiB would fit every collection model but pin 4 to 8 GiB of heap on a server running 4 to 8 commands, for 3 % on the collection. 64 MiB fits 80 % and leaves 13 % on the table.
- Peak RSS of the large runs is 2 to 5 GB at any budget (12 GB for cave-b, whose mapped textures count): the budget changes what is on the heap, not what is resident while memory is free (the handoff's finding, confirmed).

### Why not punch holes

`MADV_REMOVE` on a freed block of a shared scratch file was meant to drop its dirty pages instead of letting the kernel write them back. On btrfs it does the opposite: punching a hole writes the range's dirty pages first, then frees them. `dirty-probe.py` on scan-10m's `simplify` (HDD, 4 MiB):

| Variant | Time | Written during the run | Most dirty memory | Writes start |
| --- | --- | --- | --- | --- |
| 4 MiB | 10.3 s | 62 MiB | 1,392 MiB | at 5.0 s |
| 4 MiB with holes | 20.8 s | 1,413 MiB | 407 MiB | at 1.25 s |

Without holes, the 1.4 GB of dirty scratch pages are dropped unwritten when the file goes. With holes, they are written as blocks are freed. Across the matrix:

- **Many small primitives** (scan-10m, cave-b, the collection): more writes and slower everywhere. Collection at 4 MiB: 2.1 GB written for `simplify` instead of 0, +57 % time. scan-10m `simplify`: 1.4 GB instead of 0.2 GB, twice as slow on the HDD.
- **One big primitive** (sculpture-3m): no change; its blocks are files of their own, which holes don't touch.
- **Several meshes** (the 44M scan, 4 meshes): 2.5 GB written instead of 1.7 to 2.3 GB for `simplify`; `compress` writes 100 MiB instead of 190 MiB but takes the same time.
- **One mesh in many primitives** (the 44M tiles): 3.8 GB instead of 1.8 to 3.0 GB, and `compress` on the HDD takes 42 s instead of 17 s.
- **Draco**: the Draco model available (`statue-a/draco.glb`) is small: under 20 MiB of working set and 0.2 s, so it adds no data point on large Draco primitives.

On ext4 or XFS, punching a hole drops cached pages without writing them, so it might help there. That wasn't measured; the patch is below, should someone want to. The plain path already writes nothing on runs whose dirty pages stay under the writeback thresholds, which covers the whole collection.

### The shared-position search

`simplify`'s search for positions shared between primitives writes one 16-byte entry per vertex, in a scratch file of its own past the budget: 400 MB on the 44M scans. A variant with its entries on the heap (`MESHTINKER_SEARCH_ON_HEAP`, not committed) at 4 MiB:

- **Writes:** 2.2 to 2.6 GB on the 4-mesh scan (against 1.7 to 2.3 GB with the search in scratch), 2.2 GB on the tiles (against 3.0 GB). The differences are within the spread between runs: the search isn't the main source of writes. The arrays `simplify` holds until it writes its output are.
- **Time:** 4 to 5 s faster on both 44M scans, from not mapping and faulting 400 MB.
- **A smaller entry**, a 64-bit hash and the primitive checked against the positions on a match, takes 12 bytes, and 16 once aligned in an array of structs. Packed or split in two arrays, it saves 100 MB of the 400 MB, about 1 s on these scans, for a collision check. Not worth it.

## Figures

### Collection (56 models, SSD, one run each)

| Budget | Fit, simplify | Fit, compress | simplify time vs unlimited | compress time vs unlimited | MiB written during runs (simplify / compress) |
| --- | --- | --- | --- | --- | --- |
| 4m | 8/56 (14 %) | 10/56 (18 %) | 24.6 s (+16 %) | 17.8 s (+9 %) | 0 / 0 |
| 16m | 14/56 (25 %) | 43/56 (77 %) | 24.2 s (+14 %) | 17.8 s (+9 %) | 0 / 0 |
| 64m | 45/56 (80 %) | 45/56 (80 %) | 24.0 s (+13 %) | 16.8 s (+2 %) | 0 / 0 |
| 256m | 52/56 (93 %) | 54/56 (96 %) | 21.8 s (+3 %) | 16.4 s (+0 %) | 0 / 0 |
| 1g | 56/56 (100 %) | 56/56 (100 %) | 21.2 s (+0 %) | 16.4 s (+0 %) | 0 / 0 |
| unlimited | 56/56 | 56/56 | 21.2 s (+0 %) | 16.4 s (+0 %) | 0 / 0 |
| 4m-punch | — | — | 33.2 s (+57 %) | 22.2 s (+35 %) | 2134 / 1650 |
| 64m-punch | — | — | 25.8 s (+22 %) | 17.6 s (+7 %) | 753 / 264 |

### Large models, simplify: seconds / MiB written during the run (mean of 2 runs; 256m-punch 1 run, SSD)

| Model | Disk | Working set | 4m | 64m | 256m | 1g | unlimited | 4m-punch | 64m-punch | 256m-punch |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 44M, 4 meshes | SSD | 2786 MiB | 59.0 / 1740 | 55.0 / 2167 | 54.7 / 2267 | 52.8 / 532 | 50.2 / 1 | 55.2 / 2534 | 55.0 / 2484 | 55.5 / 2753 |
| 44M, 4 meshes | HDD | 2786 MiB | 55.5 / 2153 | 56.6 / 2088 | 59.0 / 2049 | 52.9 / 532 | 50.0 / 0 | 59.1 / 2354 | 59.1 / 2193 | — |
| 44M, 51 primitives | SSD | 1793 MiB | 44.2 / 2982 | 38.7 / 2148 | 38.4 / 2164 | 38.1 / 2011 | 34.7 / 1 | 45.3 / 3766 | 45.7 / 3706 | 45.1 / 3512 |
| 44M, 51 primitives | HDD | 1793 MiB | 38.5 / 1780 | 38.7 / 2170 | 38.5 / 2127 | 38.1 / 1322 | 34.6 / 0 | 65.7 / 3806 | 65.2 / 3749 | — |
| scan-10m 10M, 30 primitives | SSD | 442 MiB | 12.5 / 271 | 10.0 / 0 | 9.8 / 0 | 8.8 / 0 | 8.9 / 0 | 13.4 / 1413 | 13.2 / 1403 | 13.0 / 1159 |
| scan-10m 10M, 30 primitives | HDD | 442 MiB | 10.0 / 186 | 10.0 / 0 | 9.8 / 0 | 8.8 / 0 | 8.9 / 0 | 20.7 / 1414 | 21.1 / 1404 | — |
| cave-b 4.7M, 166 primitives | SSD | 543 MiB | 53.9 / 9212 | 30.7 / 8331 | 31.4 / 8288 | 25.4 / 7046 | 25.5 / 6990 | 33.2 / 8786 | 32.3 / 8733 | — |
| cave-b 4.7M, 166 primitives | HDD | 543 MiB | 75.4 / 8422 | 75.6 / 8312 | 76.0 / 8298 | 61.7 / 7017 | 61.7 / 7013 | 81.3 / 8763 | 81.9 / 8725 | — |
| sculpture-3m 3.1M, 1 primitive | SSD | 1296 MiB | 3.2 / 0 | 1.8 / 0 | 1.7 / 0 | 1.3 / 0 | 1.0 / 0 | 1.8 / 9 | 1.8 / 0 | — |
| sculpture-3m 3.1M, 1 primitive | HDD | 1296 MiB | 1.8 / 0 | 1.8 / 0 | 1.6 / 0 | 1.4 / 0 | 1.0 / 0 | 2.0 / 9 | 1.8 / 0 | — |
| statue-b, Draco (small) | SSD | 18 MiB | 0.3 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 14 | 0.2 / 0 | — |
| statue-b, Draco (small) | HDD | 18 MiB | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.3 / 14 | 0.2 / 0 | — |

### Large models, compress: seconds / MiB written during the run (mean of 2 runs; 256m-punch 1 run, SSD)

| Model | Disk | Working set | 4m | 64m | 256m | 1g | unlimited | 4m-punch | 64m-punch | 256m-punch |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 44M, 4 meshes | SSD | 1332 MiB | 17.1 / 194 | 16.7 / 0 | 16.2 / 0 | 14.6 / 0 | 14.5 / 0 | 17.2 / 103 | 16.8 / 84 | 16.4 / 74 |
| 44M, 4 meshes | HDD | 1332 MiB | 17.0 / 184 | 16.7 / 0 | 16.2 / 0 | 14.6 / 0 | 14.5 / 0 | 17.6 / 103 | 17.2 / 84 | — |
| 44M, 51 primitives | SSD | 1204 MiB | 17.2 / 1338 | 17.2 / 2176 | 16.0 / 120 | 14.1 / 0 | 13.9 / 0 | 25.9 / 3646 | 26.1 / 3422 | 23.4 / 2621 |
| 44M, 51 primitives | HDD | 1204 MiB | 17.1 / 1022 | 16.8 / 898 | 16.0 / 119 | 14.2 / 0 | 13.8 / 0 | 42.3 / 3648 | 41.3 / 3423 | — |
| scan-10m 10M, 30 primitives | SSD | 262 MiB | 3.6 / 0 | 3.5 / 0 | 3.0 / 0 | 3.0 / 0 | 3.0 / 0 | 5.8 / 821 | 5.0 / 600 | 3.2 / 7 |
| scan-10m 10M, 30 primitives | HDD | 262 MiB | 3.6 / 0 | 3.4 / 0 | 3.0 / 0 | 3.0 / 0 | 3.0 / 0 | 9.6 / 822 | 6.9 / 600 | — |
| cave-b 4.7M, 166 primitives | SSD | 325 MiB | 22.3 / 7130 | 25.4 / 7048 | 24.3 / 6820 | 24.2 / 6830 | 24.4 / 6837 | 28.8 / 7954 | 27.8 / 7686 | — |
| cave-b 4.7M, 166 primitives | HDD | 325 MiB | 61.5 / 7046 | 61.8 / 6990 | 57.5 / 6776 | 58.5 / 6797 | 59.5 / 6785 | 70.7 / 7883 | 67.8 / 7627 | — |
| sculpture-3m 3.1M, 1 primitive | SSD | 639 MiB | 2.6 / 0 | 2.6 / 0 | 2.2 / 0 | 1.8 / 0 | 1.8 / 0 | 2.6 / 12 | 2.6 / 0 | — |
| sculpture-3m 3.1M, 1 primitive | HDD | 639 MiB | 2.6 / 0 | 2.6 / 0 | 2.2 / 0 | 1.8 / 0 | 1.8 / 0 | 2.6 / 12 | 2.6 / 0 | — |
| statue-b, Draco (small) | SSD | 0 MiB | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | — |
| statue-b, Draco (small) | HDD | 0 MiB | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | 0.2 / 0 | — |

### Large models: memory and total writes (SSD, mean of 2 runs)

| Model | Command | Heap peak 4m / 256m / unlimited (MiB) | Peak RSS 4m / 256m / unlimited (MiB) | MiB written in total 4m / 256m / unlimited |
| --- | --- | --- | --- | --- |
| 44M, 4 meshes | simplify | 4 / 256 / 2788 | 4100 / 4104 / 4053 | 1972 / 2498 / 230 |
| 44M, 4 meshes | compress | 0 / 293 / 1344 | 2077 / 2153 / 2589 | 674 / 370 / 369 |
| 44M, 51 primitives | simplify | 5 / 257 / 1795 | 5082 / 5059 / 2987 | 3010 / 2190 / 26 |
| 44M, 51 primitives | compress | 9 / 269 / 1229 | 4245 / 3671 / 2425 | 1474 / 252 / 132 |
| scan-10m 10M, 30 primitives | simplify | 5 / 257 / 444 | 1795 / 1789 / 736 | 654 / 380 / 380 |
| scan-10m 10M, 30 primitives | compress | 7 / 267 / 272 | 1037 / 852 / 850 | 370 / 370 / 369 |
| cave-b 4.7M, 166 primitives | simplify | 6 / 259 / 543 | 12075 / 12027 / 10905 | 12146 / 12083 / 10739 |
| cave-b 4.7M, 166 primitives | compress | 9 / 265 / 334 | 10977 / 10957 / 11020 | 10869 / 10564 / 10550 |
| sculpture-3m 3.1M, 1 primitive | simplify | 0 / 249 / 1110 | 1434 / 1434 / 1434 | 296 / 295 / 295 |
| sculpture-3m 3.1M, 1 primitive | compress | 3 / 252 / 570 | 975 / 975 / 963 | 56 / 56 / 56 |
| statue-b, Draco (small) | simplify | 4 / 0 / 0 | 34 / 26 / 25 | 10 / 10 / 9 |
| statue-b, Draco (small) | compress | 0 / 0 / 0 | 12 / 12 / 12 | 10 / 10 / 10 |

### The shared-position search on the heap (simplify, 4 MiB budget otherwise, SSD, 2 runs)

| Model | 4m: seconds / MiB written | 4m, search on the heap |
| --- | --- | --- |
| 44M, 4 meshes | 55.1 s / 1862, 62.9 s / 1618 | 55.1 s / 2176, 54.5 s / 2614 |
| 44M, 51 primitives | 44.9 s / 3372, 43.5 s / 2592 | 38.7 s / 2208, 38.7 s / 2208 |
| scan-10m 10M, 30 primitives | 10.0 s / 124, 15.0 s / 418 | 10.0 s / 0, 10.0 s / 0 |

Notes:
- The HDD figures for the 4-mesh 44M scan's `simplify` at 4 MiB are faster than the handoff's (55.5 s against 76.4 s): the earlier runs crossed the dirty threshold, these mostly didn't. Writeback depends on what else is dirty on the machine.
- One cave-b `simplify` run at 4 MiB on the SSD took 79 s and the other 28 s; the mean (54 s) hides that. cave-b's writes are mostly its 8k textures, copied from the HDD to the output's disk, where no reflink is possible.
- The 1 GiB budget lets the 4-mesh 44M scan write 0.5 GB instead of 2 GB: its largest arrays then fit. The 51-primitive scan still writes 2 GB.

## What was changed on the branch

- `heap_array_limit` can be set at compile time (`-DMESHTINKER_HEAP_ARRAY_LIMIT=<bytes>ULL`), as the handoff suggested. Nothing changes by default.
- `MESHTINKER_ARRAY_STATS` records the most the heap budget ever held, and writes it at exit to the file `MESHTINKER_ARRAY_STATS_FILE` names. Built with an unlimited budget, it gives a model's whole working set, which is how the "fit" columns above were measured. Nothing changes by default.
- The default is now 256 MiB, as recommended.

The punch-hole patch that was measured, not committed:

```diff
diff --git a/include/meshtinker/common/workspace.hpp b/include/meshtinker/common/workspace.hpp
index fd8c56f..485def7 100644
--- a/include/meshtinker/common/workspace.hpp
+++ b/include/meshtinker/common/workspace.hpp
@@ -39,13 +39,25 @@ private:
 class ScratchBlock {
 public:
     ScratchBlock() = default;
-    ScratchBlock(std::shared_ptr<ScratchMapping> chunk, std::uint8_t* data) : chunk_(std::move(chunk)), data_(data) {}
+    ScratchBlock(std::shared_ptr<ScratchMapping> chunk, std::uint8_t* data, std::uint64_t size)
+        : chunk_(std::move(chunk)), data_(data), size_(size) {}
+    ScratchBlock(const ScratchBlock&) = delete;
+    ScratchBlock& operator=(const ScratchBlock&) = delete;
+    ScratchBlock(ScratchBlock&& other) noexcept { *this = std::move(other); }
+    ScratchBlock& operator=(ScratchBlock&& other) noexcept;
+    ~ScratchBlock() { release(); }
 
     std::uint8_t* data() const { return data_; }
 
 private:
+    // Lets go of the block. While other blocks keep its scratch file mapped,
+    // its whole pages are punched out of the file (MADV_REMOVE), so that the
+    // kernel doesn't write them back (docs/files-and-memory.md).
+    void release() noexcept;
+
     std::shared_ptr<ScratchMapping> chunk_;
     std::uint8_t* data_ = nullptr;
+    std::uint64_t size_ = 0;
 };
 
 // A private directory for a command's temporary files, after mesh2gltf's
diff --git a/src/common/workspace.cpp b/src/common/workspace.cpp
index dc66c74..5a7d1cf 100644
--- a/src/common/workspace.cpp
+++ b/src/common/workspace.cpp
@@ -110,7 +110,7 @@ ScratchBlock Workspace::block(std::uint64_t size) {
     if (size > own_chunk) {
         auto chunk = std::make_shared<ScratchMapping>(scratch("block-" + std::to_string(next_name_++), size));
         std::uint8_t* data = chunk->data();
-        return {std::move(chunk), data};
+        return {std::move(chunk), data, size};
     }
     if (size == 0) {
         return {};
@@ -121,7 +121,39 @@ ScratchBlock Workspace::block(std::uint64_t size) {
     }
     std::uint8_t* data = chunk_->data() + chunk_used_;
     chunk_used_ += size;
-    return {chunk_, data};
+    return {chunk_, data, size};
+}
+
+ScratchBlock& ScratchBlock::operator=(ScratchBlock&& other) noexcept {
+    if (this != &other) {
+        release();
+        chunk_ = std::move(other.chunk_);
+        data_ = other.data_;
+        size_ = other.size_;
+        other.data_ = nullptr;
+        other.size_ = 0;
+    }
+    return *this;
+}
+
+void ScratchBlock::release() noexcept {
+#if defined(MESHTINKER_MMAP) && defined(MESHTINKER_SCRATCH_PUNCH)
+    // Only while the file stays mapped for other blocks: unmapping an
+    // unlinked file drops its pages already.
+    if (chunk_ && chunk_.use_count() > 1 && size_ > 0) {
+        static const auto page = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
+        const auto start = reinterpret_cast<std::uintptr_t>(data_);
+        const std::uintptr_t first = (start + page - 1) / page * page;
+        const std::uintptr_t last = (start + size_) / page * page;
+        if (last > first) {
+            // Best effort: on failure the pages are written back, as before.
+            static_cast<void>(::madvise(reinterpret_cast<void*>(first), last - first, MADV_REMOVE));
+        }
+    }
+#endif
+    chunk_.reset();
+    data_ = nullptr;
+    size_ = 0;
 }
 
 ScratchMapping Workspace::scratch(const std::string& name, std::uint64_t size) {
```

## Suggested text for TODO.md, "flexible memory management"

TODO.md is gitignored here, so this is the text to put in place of that section:

> Commands keep per-vertex and per-face working arrays on the heap up to a constant budget, and in mapped scratch files beyond (docs/files-and-memory.md). Measured on the corpus (docs/experiments/heap-budget-results.md): raise `heap_array_limit` from 4 MiB to 256 MiB. 93 % of the collection then simplifies without scratch, and spilling costs 3 % instead of 16 %. Four to eight parallel commands keep at most 1 to 2 GiB on the heap. Large models still spill, and that's what lets several run at once. Punching holes in freed scratch blocks makes btrfs write them: don't. On ext4 or XFS it might help; measure it there before adopting it. Meta-commands still pick commands by their memory use: `simplify` and `compress` spill beyond the budget, and other commands may not.
