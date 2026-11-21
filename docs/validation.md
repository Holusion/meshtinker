# Validation

How to check what a meshtinker command produces: which models to run it on, which tool answers which question, and what the reference figures are. Everything here is for development only; the shipped binaries never depend on it.

## Setup

```sh
npm ci --prefix tools        # Node 22: pinned validator, gltf-transform with its decoders, sharp
tools/fetch-assets.sh        # Khronos sample models at a pinned commit, into test/assets/khronos (28 MB, gitignored)
node tools/corpus.mjs check  # which corpus models are present
```

System programs: `ktx` (KTX-Software) to generate fixtures, `jq` for `fetch-assets.sh`, and `python3` for `monitor.py`.

gltf-transform is pinned to what eCorpus's server runs (4.5.1, with draco3dgltf 1.5.9 and meshoptimizer 1.3.0), so `compare.mjs` reads models as eCorpus does. The validator is gltf-validator 2.0.0-dev.3.10.

## Which tool answers which question

The checks don't ask another tool for the right answer. They test properties of the output itself: it's valid, it keeps what it promises to keep, it agrees with figures known by construction, and it uses little memory.

| Question | Command | Passes when |
| --- | --- | --- |
| Is the output valid glTF? | `node tools/validate.mjs OUT --against IN` | `newIssues` is empty: no error or warning code that the input didn't already have |
| Did `import` keep everything it promises to keep? | `node tools/compare.mjs IN OUT` | `equal: true`. Exact mode: same scene tree, materials, image bytes, decoded vertex values and indices, animations, unknown extensions and extras |
| Is the geometry the same, ignoring vertex order and welding? | `node tools/compare.mjs IN OUT --geometry triangles` | `equal: true`. For lossless reindexing only; the tolerance absorbs float noise, not lossy compression |
| Are `inspect`'s figures right? | ctest, against each fixture's `expected.json` | Equal, within the tolerance `expected.json` states |
| Did a change alter `inspect`'s figures? | `check-commands.mjs`, against the snapshots | Equal to the snapshot, or the diff of `--update-snapshots` is explained by the change |
| All of the above for `inspect` and `import`, over many models | `node tools/check-commands.mjs [--id P] [--covers T] [--without T] [--source S] [--fast]` | Every line `ok`. It also checks that `inspect` and `import` accept the same files, and that inspecting `import`'s output gives `import`'s report. `--fast` skips the validator and `compare.mjs`, for `large` models |
| Does `placeholder` overlay its source? | ctest (`overlays_every_fixture`); on other models, `meshtinker-inspect --json` of the source and of the placeholder | `scene.bounds` within one `cellSize` (from the placeholder's `--json` report). Points and lines are left out, so models with them differ |
| Does a lossy geometry encoding (`compress`) draw the same model? | `node tools/compare.mjs IN OUT --geometry world --tolerance T`, with T about 1/16,000 of the model's longest side (`inspect`'s `scene.bounds`); ctest (`test/compress/compress_test.cpp`) | `equal: true`. Triangles match in world space, so quantization that moves scale into node transforms compares; normals within `--attribute-tolerance` (0.02) |
| Does `simplify` stay within its error and keep tiles sealed? | ctest (`test/simplify/simplify_test.cpp`) on `scan-tiles`; on other models, `validate.mjs --against` and `monitor.py` | Every output vertex is an input vertex with all its attributes; positions shared by two primitives are in both; every input vertex is within 2.5 × the error of the output (meshoptimizer's error is an estimate, [simplify.md](simplify.md#how-far-the-error-is-from-the-distance)) |
| Do chains record, link and publish stages right? | ctest (`test/chain/chain_test.cpp`, `test/commands/chain_test.cpp`): the commands as built, on fixtures | Stages numbered and linked by `parent`, reports and `lossy` carried, unchanged files hard links to the parent's, nothing left by a failure |
| Does `ktx` encode each image for its slots, and keep the model? | ctest (`test/ktx/ktx_test.cpp`): basisu's transcoder decodes each file; on other models, `ktx validate --gltf-basisu` on each `.ktx2`, `validate.mjs --against` and `compare.mjs IN OUT` | Codec, transfer function, primaries, alpha and full mips as [ktx.md](ktx.md#codec-per-slot) says, PSNR of at least 25 dB on the fixtures; every file valid; `newIssues` only the `KHR_texture_basisu` codes of [Known validator differences](#known-validator-differences); `compare.mjs` differs only in the encoded textures' `image` and `mimeType` |
| Does `uv` keep what every triangle samples? | ctest (`test/uv/uv_test.cpp`): old textures at old UVs against new textures at new UVs, bilinear, at random points in every triangle; on other models, `validate.mjs --against`, `inspect` occupancy before and after, `tools/experiments/uv/measure.py` | Difference under 0.01 of 255 on the fixtures; `newIssues` empty; `compare.mjs` differs only in the repacked textures' images and in `TEXCOORD_n` |
| Does `export` write the stage as it is? | `node tools/validate.mjs OUT --against IN` and `node tools/compare.mjs IN OUT` on the export of an imported chain | `newIssues` empty and `equal: true`, as for `import` |
| How much memory does it use? | `python3 tools/monitor.py out/run -- build/meshtinker-inspect --json FILE` | `peak_anon_kib` stays small and roughly flat as models grow; RSS from mapped files is fine |

How eCorpus's current figures map to the report, and where they differ, is in [inspect-report.md](inspect-report.md#mapping-from-ecorpus).

## The corpus

`test/corpus.json` lists every model used for validation, from three sources:

| Source | Where | In CI | Use |
| --- | --- | --- | --- |
| `fixture` | `test/data`, committed | ctest, and `check-commands.mjs` in the end-to-end workflow | Tiny generated models, one case each, with known answers. The only models ctest may use. |
| `khronos` | `test/assets/khronos`, fetched | `check-commands.mjs` in the end-to-end workflow | Khronos glTF-Sample-Assets: spec edge cases and realistic small models. |
| `local` | listed in `test/corpus.local.json`, on each developer's machine | never | Real scans, including the large models used for memory measurements. Read in place, never modified or copied. |

Validation runs on models anyone can reproduce: generated fixtures and the pinned Khronos samples, with synthetic models preferred. Real data, the `local` models, is for manual testing and development rounds: running a new command on actual scans, and measuring time and memory at full size. No check needs it to pass, so every tag a check relies on has a fixture: `photogrammetry`, `udim`, `spec-gloss` and `invalid` have one (`scan`, `udim`, `spec-gloss`, `invalid`) besides their local models. Only `large` has local models alone, until generated ones exist.

`test/corpus.local.json` is gitignored, so that private models' names and paths stay on the machine. `tools/corpus.mjs` merges it into `test/corpus.json` when it exists. It has the same shape, with the `local` source's `root` (an absolute folder) and its assets:

```json
{
  "sources": { "local": { "root": "/path/to/models" } },
  "assets": [
    { "id": "local/scan-large", "source": "local", "path": "scan/scan.gltf", "covers": ["gltf", "large", "photogrammetry"] }
  ]
}
```

Select models by tag instead of hard-coding paths:

```sh
node tools/corpus.mjs paths --covers meshopt --without large   # one path per line
node tools/corpus.mjs list --source fixture                     # id<TAB>path
node tools/corpus.mjs tags                                      # every tag with its count
node tools/corpus.mjs paths --covers large                      # memory measurements
```

Tags, by what they cover:

- **Containers and references:** `gltf`, `glb`, `glb-images` (images in the GLB binary chunk), `data-uri`, `multi-buffer`, `uri-escaping` (spaces, `%`, `#`, non-ASCII), `ref-outside`, `ref-absolute`, `ref-symlink`.
- **Scene and geometry:** `minimal`, `empty`, `multi-scene`, `no-default-scene`, `orphans` (meshes and nodes no scene uses), `node-transforms`, `negative-scale`, `shared-mesh`, `shared-accessor`, `shared-buffer-view`, `interleaved`, `sparse`, `primitive-modes`, `non-indexed`, `morph`, `skin`, `animation`, `gpu-instancing`.
- **Compression:** `meshopt` (EXT_meshopt_compression), `khr-meshopt` (KHR_meshopt_compression), `meshopt-fallback`, `draco`, `quantized` (KHR_mesh_quantization).
- **Textures:** `png`, `jpeg`, `webp`, `ktx2`, `alt-source` (a texture with a core source and an extension source), `samplers`, `texture-transform`, `multi-uv`, `texcoord-origin`, `shared-image`, `unused-image`, `all-slots`, `normal-map`, `udim`, `occupancy` (hand-computed answers).
- **Extensions and validity:** `unknown-ext`, `unknown-required`, `missing-image` (an image file is missing: rejected for a base color, a warning otherwise), `extras`, `spec-gloss` (KHR_materials_pbrSpecularGlossiness, archived but still exported), `invalid` (the input already has validator errors).
- **Scale:** `photogrammetry`, `tiled-primitives` (one mesh in several primitives that share borders, as tiled scan exports write them), `tiled-meshes` (one mesh per tile, all drawn in one space), `large` (hundreds of MB to GB).

## Fixtures

Each `test/data/<name>/` folder holds:

- the model;
- `expected.json`: what is true by construction, written by the generator: counts, bounds (with a tolerance where floats are involved), image facts and hand-computed occupancy, with the reasoning in `why` and `notes`. `images` lists every image of the input by index; one marked `dropped` must not be in the report. **It is authoritative.**

`scan` is a small synthetic photogrammetry scan, from the generator's `scanBuilder`. It's a bumpy open band cut into charts on one JPEG atlas, with seams duplicated, uint32 indices and a node transform. Its size, seed and chart count are parameters. Its occupancy isn't computed by hand: `expected.json` holds objtools' figure, which meshtinker matched independently.

Regenerate with `node tools/fixtures/generate.mjs [NAME...]`. Output is deterministic, so a rerun must leave `git status` clean. To add a case, add a function to the generator and an entry to `test/corpus.json`, then commit the generated files. Fetch a new Khronos sample instead only when a generated fixture can't reasonably cover the case.

The hand-computed occupancies in `occupancy/expected.json` (nearest and linear filtering, `REPEAT` wrapping, `KHR_texture_transform`) were checked against objtools, which reproduces every one exactly.

## Snapshots

`check-commands.mjs` compares `inspect`'s report on each Khronos model with a snapshot in `test/snapshots/khronos/<name>.json`: the whole report but the `tool` field, diagnostics included. Integers compare exactly, other numbers within 1e-9 relative. A Khronos model without a snapshot is a problem.

The snapshots were taken when every figure agreed with objtools and with eCorpus, apart from the differences inspect-report.md lists. They don't say a figure is right, only that it hasn't changed. When a change is meant to alter figures, rewrite them and review the diff with the change:

```sh
node tools/check-commands.mjs --source khronos --update-snapshots
git diff test/snapshots
```

For `local` models, the same option writes snapshots to `test/assets/snapshots` (gitignored: figures of private models stay on the machine). Write them before a change and check after, to see what it does to real scans. A local model without a snapshot is skipped.

## Known validator differences

When the validator reports one of these, the output isn't at fault.

- Doesn't recognize KTX2 images: `warning:IMAGE_UNRECOGNIZED_FORMAT` on every `.ktx2` image, the Khronos samples included.
- Reports extensions it doesn't know (meshopt, KHR_meshopt) as `info:UNSUPPORTED_EXTENSION` and doesn't check their data. A buffer only meshopt extensions refer to is therefore `info:UNUSED_OBJECT`: `meshopt-quantized` after `import`, whose `.bin` then holds only compressed data. `check-commands.mjs` doesn't count that info for meshopt outputs.
- Warns `VALUE_NOT_IN_LIST` on the `mimeType` of a WebP or KTX2 image (`export` of `alt-sources`, every `ktx` output): core glTF lists only PNG and JPEG, and `EXT_texture_webp` and `KHR_texture_basisu` require `image/webp` and `image/ktx2` there.
- Doesn't support `KHR_texture_basisu` (`info:UNSUPPORTED_EXTENSION`), so an image only that extension refers to can be `info:UNUSED_OBJECT` (`ktx` of `occupancy`). Check KTX2 files with `ktx validate --gltf-basisu` instead.
- Warns `MESH_PRIMITIVE_GENERATED_TANGENT_SPACE` for normal maps without tangents (`occupancy`). That's legal glTF.

## Limits of the tools

- `compare.mjs --geometry world` compares what the scene draws, in traversal order, not the node tree: names, extras and node structure go unchecked, and animation targets match by node name. Triangles are matched greedily, each to its closest unused candidate.
- `compare.mjs` reads with gltf-transform, so it can't open KHR_meshopt_compression files, or files with an unknown extension in `extensionsRequired` (`unknown-required`): it exits with 2. It also holds whole models in memory: fine up to a few hundred MB, not for `large` models.
- `validate.mjs --against` counts only errors and warnings as new issues. Infos such as `info:UNUSED_OBJECT`, which reveal objects left behind by a rewrite, show in each side's `codes` but not in `newIssues`: compare the two `codes` objects when a command should leave nothing unused.
- Unknown extensions are compared as multisets of their JSON values. An extension whose value holds an index (`unknown-ext` has one) shows up as lost and added when indices are renumbered: decide case by case.
- Buffer view `extras` are not compared, because `import` rewrites buffer views by design.

## Recommendation for end-to-end validation

Drawn from the field notes below, which so far cover `inspect` and `import`. Revisit it once a lossy command (simplification, texture compression) has been through it.

**Keep, in this order of value:**

1. **The reference implementation's own tests, ported with its code**, as `test/analysis/coverage_test.cpp` ports objtools'. Its brute-force comparison over 8,000 random triangles is what made occupancy right the first time. When a command follows another tool, port that tool's tests first.
2. **Negative inputs in ctest:** wrong file types, wrong versions, refused references. They found a reader bug no other check saw (cgltf accepting any JSON object), and no corpus model exercises them.
3. **Cross-command consistency:** `inspect` on `import`'s output must give `import`'s report. It's cheap, runs in ctest for every fixture, and checks index renumbering, which no figure from outside sees.
4. **Time and memory on the `large` models** (`monitor.py`), against the previous build. It found the only real performance problem so far (4× slower than objtools), which every correctness check passed. Record `elapsed_seconds` along with `peak_anon_kib`.
5. **`validate.mjs --against`** on every output, comparing infos (`UNUSED_OBJECT`) as well as errors and warnings, with the known differences listed above.
6. **Hand-computed fixtures** (`expected.json`) **and snapshots.** They confirmed rather than found, but they're what makes a figure trustworthy to eCorpus, and they cost nothing.

**No reference tool.** Comparing with objtools and eCorpus helped while porting `inspect`, and the agreed figures live on as snapshots. It never found a bug that the ported tests or the fixtures missed, and it cost a known-differences list per tool. Lossy commands have no single right answer to compare with anyway: they will be checked by properties of their output, measured from the data.

**Run less often:**

- **The whole local corpus** (`check-commands.mjs` on 122 models: about 10 minutes, mostly the validator and gltf-transform). It found no bug that the fixtures or Khronos models didn't. It's a development round, not a gate: run it when a change to reading or writing is ready, and when a new command first meets real scans.
- **`compare.mjs`**: no bug found, but it's the only check of the full content of an output. Keep it for every command that rewrites files. Avoid gltf-transform's `equals` in it: it compares storage details and raised false alarms on every animated model.

## In CI

- **Every push** (`.github/workflows/ci.yml`): ctest on the fixtures, in release builds for x86_64 and arm64 and a sanitizer build, all with the reference toolchain and warnings as errors; the `ldd` check. clang-tidy runs only locally.
- **Nightly, on pull requests and on demand** (`.github/workflows/end-to-end.yml`): `check-commands.mjs --source fixture --source khronos`. It runs the validator, `compare.mjs`, the `inspect` round trip and the snapshots, over the fixtures and the Khronos models (fetched at the pinned commit, cached). It takes a few seconds once the tools are installed. `summary.json` is kept when it fails.

## Field notes

What each check actually caught during development, and what it cost. They will decide what proper end-to-end validation reaches for, and what it leaves out. Add an entry when a check finds a real bug, misses one that another check or a manual look found, gives false alarms, or costs more than it's worth. Keep entries short and concrete: the check, the change being checked, what happened, and the lesson.

| Area | Check | What happened | Lesson |
| --- | --- | --- | --- |
| writer | ctest negative case (expected.json read as a model) | cgltf accepted any JSON object as glTF: `expected.json` parsed and validated as an empty model. Fixed by sniffing the first byte and checking `asset.version`. | Keep negative inputs (wrong file types, wrong versions) in ctest: no corpus model exercises them. |
| writer | `compare.mjs` on the Khronos models | Every animated model reported `animations[0] differ` after a lossless rewrite: gltf-transform's `Animation.equals` follows accessors to their `Buffer`, whose `uri` changes when files are repackaged. A false alarm. Fixed by comparing channels, targets and decoded keyframes. | Compare decoded values, never gltf-transform's `equals`, which includes storage details. Check a comparison tool against a known-equal pair and a mutated one before trusting it. |
| writer | `validate.mjs --against` | GLB images extracted to files leave their buffer view unused: `info:UNUSED_OBJECT`, which `newIssues` ignores because it's an info. | For commands that drop or move objects, compare the `codes` of both sides, infos included. |
| writer | Round trip of every fixture, Khronos and local model (98), then `validate.mjs --against` and `compare.mjs` | No new issue and all equal once `compare.mjs` was fixed. About 10 minutes, mostly gltf-transform and the validator on the photogrammetry scans. | Worth running once per change to reading or writing. It found no writer bug, so a rewrite that keeps view indices is low risk. Reordering and pruning in `import` will be the real test. |
| writer | `monitor.py` on the `large` models | Heap stays under 400 KiB on a scan with a 1.3 GB `.bin` and a 710 MB GLB. RSS is the mapped files, as intended. | Cheap, and the only check that sees memory. Run it whenever reading or writing changes. |
| inspect | ctest against `expected.json`, then oracles over 122 models (`check-commands.mjs`) | Every figure matched on the first run, occupancy included: the objtools port came with objtools' own unit tests (brute-force overlap included), which were ported first. Bounds match eCorpus within float noise on every model it can read. | Porting the reference implementation's tests along with its code is what made the first run right. Hand-computed fixtures and oracles then only confirmed it. Cheap to keep, but they found nothing on their own here. |
| inspect | `monitor.py` timing against objtools on a 1.3 GB scan with 16 UDIM textures | `inspect` took 52 s against objtools' 13 s, with correct figures. Each of a material's 4 images was rasterized separately, from identical UV draws. Grouping images drawn the same way into one coverage map brought it to 15 s. | Time the large models against the reference tool, not only their memory. No correctness check can see a 4× slowdown, and the small models hide it. |
| import | `validate.mjs --against` on `meshopt-quantized` | `info:UNUSED_OBJECT` on the `.bin` after import: the validator doesn't read meshopt extensions, so a buffer only they use looks unused. False alarm, now left out by `check-commands.mjs`. | Validator infos on files with extensions it doesn't support need a known-differences list, as here. |
| import | `compare.mjs` on the corpus | Can't open `KHR_meshopt_compression` or unknown required extensions (3 models): those imports are checked only by the validator, `inspect` round trip and oracles (snapshots now). | A comparison tool independent of gltf-transform's extension support is missing for these. |
| import | `inspect` on `import`'s output against `import`'s report (ctest and `check-commands.mjs`) | Always equal. It pins down index renumbering in the report, which no oracle sees. | Keep: cheap, and the one check that ties the two commands together. |
| Draco | Sanitizer build of ctest, which decodes the Draco fixture | Clean, but it was the first sanitized run of vendored Draco code. A stale sanitizer build (a test that didn't compile stopped the build) briefly reported failures from old code. | Check that the build succeeded before reading test results; `cmake --build ... && ctest` in one command avoids it. |
| Draco | `monitor.py` on the local Draco models | Heap peaks at 16 MB for 233k vertices: Draco decodes a whole primitive into memory, about 70 bytes per vertex. | The memory check is what shows a vendored decoder's cost. Run it on every new decoder. |
| Draco | Bounds against eCorpus (`check-commands.mjs --covers draco`) | Equal on all 7 Draco models, but eCorpus uses the same Draco version, so it confirms the integration rather than the decoding. | An independent oracle for a decoder needs a different implementation, or the uncompressed source (`khronos/Duck-glb` against `Duck-draco`: same bounds and occupancy within quantization). |
| Draco | CI build | Failed on missing Draco headers that every local build and test had passed with: the machine's `libdraco-dev` headers in `/usr/include/draco` filled the gaps silently. | CI on a clean machine is the check for vendoring. Locally, list dependencies (`g++ -MM`) and grep for system paths. |
| validation | objtools and eCorpus figures for every model (`check-commands.mjs`) | They confirmed every figure and found no bug that the ported tests or `expected.json` missed. They cost an objtools build, a copy of eCorpus's code kept faithful, and a list of known differences per tool. Retired; their agreed figures became the Khronos snapshots. | A reference tool earns its keep while porting it. Afterwards, freeze its agreed figures. |
| CI | `check-commands.mjs` in CI, on fixtures and Khronos models | A model without oracle figures used to pass silently, since figures were found missing and treated as empty, and so did an empty selection (a failed fetch). Both are problems now. The whole run takes about 4 s for 64 models. | A check that can silently compare against nothing passes in CI forever. Make missing inputs a failure, and mutate one expected figure to see it fail. |
| normalized input | ctest: the normalized document against `import`'s output, over every fixture | Passed on the first run. Breaking the buffer renumbering and the image mapping on purpose made it fail on most fixtures. | Check a test that passes at once by breaking the code it covers. |
| placeholder | Bounds of the placeholder against the source's (`inspect`), over the fixtures and Khronos models | Found a misplaced skinned model (BrainStem drawn Z-up): the frame came from the skin, but no node carried its rotation into the output. No fixture had a skinned mesh under a transformed root; `skinned` now does. | Cheap and catches placement bugs. Run it on the Khronos models for any change to transforms: they hold the skinned and animated cases. |
| placeholder | ctest mutations: reversed winding, no per-node transform to the frame, the skinned-frame bug | Each made a test fail (`cube`, `overlays_every_fixture`). Using the skinned node itself as the frame went unnoticed, but it gives the right answer whenever the mesh node's world matrix equals joint × inverse bind matrix, as in every fixture and Khronos model. | A fixture where the two differ would need `inspect` to ignore a skinned node's transform as the spec says; it doesn't today (TODO.md). |
| placeholder | `monitor.py` on the `large` models | 44M faces in 6.6 s (uncached) at resolution 16 with 0.6 MB of heap, and 18 MB at 128 (4 threads' grids). | Matches the grid sizes. The validator found nothing on any output. |
| simplify | `monitor.py` on tiled local scans (16 and 634 primitives) | Heap at 53 MB and 127 MB: each working array was under the 4 MiB heap limit, but a mesh's primitives are held together, so the whole model was on the heap. Fixed by making 4 MiB the total of all working arrays and meshoptimizer's allocations, with shared scratch files beyond: 4.7 MB and 15 MB (the JSON of 634 primitives, held a few times). | A per-allocation limit doesn't bound the heap. Measure models with many small primitives, not only large ones. |
| simplify | ctest: distance from each input vertex to the simplified surface, brute force, on `scan-tiles` | The farthest vertex is 2.1 times meshoptimizer's error away at 0.01: its quadric error is an estimate, not a bound. Not a bug, but `--error` had been documented as a limit on distance. | Measure a lossy command's error from the data before documenting what its option means. |
| simplify | `validate.mjs --against` on the fixtures and 4 tiled scans | No new issue. | Cheap for geometry rewrites; the property tests carry the content checks, since `compare.mjs` has no lossy mode. |
| simplify | `monitor.py` on the 44-million-face scan (4 primitives, 8.2 GB with textures) | 76 s to 491,000 faces, heap 4.5 MB. One thread: meshoptimizer's allocator hook is process-wide. | The baseline for a parallel version. |
| fixtures | Rerunning `generate.mjs` | `alt-sources/ktx-only.ktx2` changes on every run with `ktx` 4.4.2, so the rerun doesn't leave `git status` clean. Unrelated to the change being made; restored by hand. | The KTX fixture needs a deterministic encoding (or a committed file the generator doesn't rewrite). |
| simplify, compress | `meshtinker-compress` report on the 44-million-face scan, then a count of positions shared between meshes (a scratch script) | The scan is 4 meshes, one per UDIM tile, on nodes without transforms, sharing up to 77,000 border positions: `simplify` locked borders within a mesh only, so the tiles could crack, and `compress` gave each mesh its own grid. No check had seen it: `scan-tiles` puts its tiles in one mesh. Fixed by grouping meshes drawn in one space (`gltf::meshSpaces`); every shared position is still shared after `simplify`, and `scan-meshes` covers the case. | Look at how real scans are split before deciding what a border is. A fixture built from the generator's idea of a scan hides layouts real exports use. |
| compress | `compare.mjs --geometry triangles` on quantized output | Every quantized model differed: quantization moves the scale into the node transform, and the triangle mode compares local values. Added `--geometry world`. Its first version matched each triangle to the first candidate within the tolerance, and found a Draco model different from itself; it now takes the closest. | Compare a tool with itself on a real model, not only on fixtures: dense meshes put several triangles within the tolerance. |
| compress | `compare.mjs --geometry world` on the fixtures | Found two bugs: rewritten accessors lost their `extras` (`unknown-ext`), and quantizing skin weights forced their sum to one where the input's was 0.8 (`invalid`). Also flagged strips turned into lists, by design, until the comparison expanded strips. | The content comparison catches what the validator can't: lost data and changed values. |
| compress | Size against glTF-Transform 4.5.1's `meshopt({level: "high"})`, through the pinned `tools/` | Within 7 % either way (5 to 7 % smaller on real models, 13 % larger on `scan-tiles`). A first script loaded gltf-transform's CommonJS build next to `io.mjs`'s ESM one: the transform silently did nothing. The difference is how positions are stored: our 14-bit grid index unshifted, where glTF-Transform scales it to the full 16-bit range. Shifting ours by two bits gives its sizes within 0.01 %. | Run reference scripts as ESM from `tools/`, and check the output's accessor types before trusting a size. |
| validation | `monitor.py` with stdout and stderr hidden | Reusing a run name made it refuse to overwrite, with a message that the redirect hid: the figures read were the previous run's. | Give every run a new name, and keep monitor's stderr. |
| memory | `simplify` and `compress` built with a working-array heap budget of 4 MiB, 256 MiB and unlimited, on the 44-million-face scan and a 10-million-face scan (30 primitives), outputs and workspace on the SSD and on a spinning disk, inputs in the page cache, two runs each | 44M `simplify`: 61 s at 4 MiB on the SSD, 76 s on the spinning disk, 53 s unlimited on either, with 2.8 GB of heap. At 4 MiB the kernel wrote 5 GB of scratch pages back to disk during the run, more than the working set (pages are rewritten); unlimited wrote only the output. 256 MiB is too small to change that. Peak RSS is about 4 GB in every case: mapped scratch doesn't use less memory while RAM is free, it only lets the kernel reclaim it. `compress`: 18 s against 16 s; the 10M scan: 10.7 s against 9.8 s. | Spilling to scratch files costs time on a spinning disk and disk writes everywhere. It only pays under memory pressure, so the budget should follow the memory available rather than be a small constant. |
| simplify | Phase timings on `Lascaux_seamed_8k` (166 primitives), after it lost 0.03 % of its faces | Locking wasn't the cause (1.35 of 14.1 million vertices locked): the model is a triangle soup, three vertices per triangle. So is the 634-primitive scan, whose 3 % had been put down to locking in docs/simplify.md. Its 47 s were mostly copying 10.9 GB of textures. | Check a model's structure (vertices per face) before explaining a figure with the code's behaviour. |
| chaining | ctest (`test/chain/chain_test.cpp`), a failed `import` into a directory it created | The directory stayed: the stage writer, which removes it, was destroyed before the `Outputs` that still held the stage's files, so the directory wasn't empty yet. Fixed by removing a directory the run created whole. | Test failure paths with the objects in the order a caller declares them, not the order the implementation expects. |
| chaining | `validate.mjs --against` on `export`'s GLB of every fixture | One false alarm: `VALUE_NOT_IN_LIST` on the `image/webp` and `image/ktx2` mime types `alt-sources` needs once its images are embedded. Every other fixture had no new issue, and `compare.mjs` found every GLB equal to its source. | Listed under known validator differences. Validator plus exact `compare.mjs` over all fixtures is a cheap check for a new container writer. |
| chaining | `monitor.py` on `import`, `compress`, `inspect`, `export` and `export --remove-chain` of the 44-million-face scan as a chain, on Btrfs | Peak heap under 1 MB for each. `export` of the stage as `.gltf` took 0.2 s: its files are reflinks; with `--remove-chain` they are hard links to the stage's. A `SIGTERM` 3 s into `import` left nothing, the chain directory included. | Reflinks make copies of unchanged files free on Btrfs and XFS; on ext4 they fall back to `copy_file_range`, still outside the process. Measure on ext4 too before relying on `export` timings. |
| ktx | `ktx validate --gltf-basisu` on the output of `occupancy` | A real bug: every UASTC file was non-conformant, because basisu writes BT.709 primaries with the linear transfer function, where `KHR_texture_basisu` asks for unspecified ones. Neither the glTF validator nor the transcoder would have caught it: three.js reads the file either way. Now fixed in `encode.cpp`, and checked by ctest. | Run it on every `ktx` change: it is the only check of the KTX2 container against the extension. |
| ktx | ctest PSNR against the source on the `textures` fixture | Two false alarms, both the fixture's: a 16-bit PNG whose pixels were noise (sharp byte-swaps, then converts 16-bit raw input as a color space, so the PNG is now written by hand), and ETC1S at 26 dB on a resized 32×20 pattern with hard edges. The threshold is 25 dB: it catches broken channels or transfer functions, not small losses. | Look at the source pixels before suspecting the encoder. |
| ktx | `validate.mjs --against` and `compare.mjs` on DamagedHelmet and a 4k scan (`01-akhenaton-nefertiti`) | Only the known `KHR_texture_basisu` codes; geometry and materials equal. 16 s and a 365 MiB peak for DamagedHelmet's five 2k images, 15 s and 1.05 GiB for the 4k scan texture, on a lightly loaded machine. | No viewer has opened a `ktx` output yet: open one in eCorpus's viewer before merging. |
| uv | ctest sampled comparison (old texture at old UV against new texture at new UV) on `atlas` and `udim` | A real bug: up to 1.3 of 255 off, where xatlas had placed charts against the atlas's edge, so the texels bilinear filtering reads around them fell outside the image. Fixed with a margin of 2 texels on every side. Writing the check also showed the bake copied texels within 1 texel of each triangle where bilinear filtering reads up to √2 away. | The cheapest check that a whole-texel copy is exact: keep its threshold near 0, where any resample or misplaced chart shows. |
| uv | a debug print on the `scan-tiles` fixture | xatlas stretches every chart until its extents are whole texels, with no option to turn it off: no move was a plain translation. `uv` now moves charts unstretched by their minimum corner, without block alignment. | Check a library's output against its documented options before building on it. |
| uv | `measure.py` over the 56 collection models | A real bug: one scan got a 41-billion-texel atlas, because it packed in texels of the group's smallest image, a 16² flat map beside 4k ones. The no-gain threshold hid it; no test could. Now packed in the largest image's texels, with flat small images left out. Also found: 159 groups left as they were for a few isolated zero-area UV triangles, now moved with the nearest chart. | Run the measurement script on the corpus after each change to `uv`: its outliers find bugs the fixtures are too small for. Don't rebuild the binary during a run: the run dies when the file is replaced. |
| uv | `compare.mjs` on a repacked scan | A normalized-short `COLOR_0` came back as float32: `uv` rewrote every attribute plain, doubling that attribute's size. Uncompressed primitives now keep their other accessors and get a new `TEXCOORD_n` only. | Compare even when only one attribute should change. |
| memory | `heap-budget.sh` matrix: 56 collection models and 6 large ones, 4 MiB to unlimited, SSD and HDD (docs/experiments/heap-budget-results.md) | Below the writeback thresholds no budget writes scratch to disk: the collection's runs write only their outputs at any budget, and what a small budget costs is mapping and faulting scratch pages, +16 % on `simplify` at 4 MiB. On the 44M scans 2 GB gets written at any budget from 4 MiB to 1 GiB. | Time and sectors written are both needed: on small models the cost is CPU, on large ones disk. `MESHTINKER_ARRAY_STATS` measures a model's working set directly, without inferring it from writes. |
| memory | `madvise(MADV_REMOVE)` on freed scratch blocks, measured with `dirty-probe.py` | A false lead: on btrfs, punching a hole writes the range's dirty pages before freeing them. Notre Dame's `simplify` wrote 1.4 GB instead of 62 MiB, and took twice as long on the HDD. | Probe the kernel's behaviour on the file system in use before relying on what a call should do: the man page doesn't say. |
| memory | the shared-position search with its entries on the heap | Its 400 MB scratch file accounts for less than the spread between runs in the 44M scans' writes, and 4 to 5 s of their time. | Measure a suspected cause in isolation before optimizing it. |
