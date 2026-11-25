# optimize

How `meshtinker optimize` turns a model into an optimized one in one step, and why. The code is `src/commands/optimize/`: `plan.cpp` decides which steps run, and everything else runs them.

```bash
meshtinker optimize in.glb out.glb                        # slow preset, quality High, KTX2
meshtinker optimize in.gltf out.glb --quality Low
meshtinker optimize in.glb out.glb --preset fast          # a temporary model, quickly
```

## How it runs

`optimize` is a meta-command ([chaining.md](chaining.md#meta-commands)): it runs the other commands as child processes on a chain of its own, as a user would by hand.

1. It creates its workspace beside the output (or in `--tmp-dir`), and runs `import in <workspace>/chain`.
2. It runs the plan's steps on the chain, each with `--json`, reading each report before deciding the next step.
3. It runs `export <workspace>/chain out`, which writes a `.glb` or `.gltf` by the output's extension: the last stage, or import's when [the guard](#guard) keeps the input.
4. It removes the workspace, chain included, unless `--keep-tmp` is given: the chain then stays for debugging, each stage with its `stage.json`.

- **Finding the commands.** `meshtinker-<name>` next to `meshtinker-optimize` first, then on `PATH`: the wrapper's lookup, in `common` (`common/subcommands.hpp`).
- **Output.** Each step's stderr passes through, so warnings appear once, from the command that raised them, as it runs. Optimize logs each step at `info` as it starts (`Running simplify`) and ends (`simplify took 12.3 s`, or `failed after`), and its options at `debug`; it passes `--log-level` and `--log-format` on to the steps ([logs.md](logs.md)). The text report lists the steps with their times, then the bytes and the estimated GPU memory before and after. With `--json`: `settings`, `machine.memory`, `steps` (`command`, `argv`, `exitCode`, `signal`, `seconds`, and the step's own `report`), `input` and `output` (`byteSize` and `gpuBytes`, the [estimate](#gpu-budget); `output.stage`, the stage exported), `seconds`, and `diagnostics`, every step's tagged with its `step`.
- **Failures.** A step that fails, or is killed by a signal, is left out: the chain is as it was before it, since a failed command publishes no stage, and the plan goes on from there. `optimize` warns `step-failed`, with the step and its error, and its report keeps the step with its exit code or signal. A step that can't make its output (out of memory, a decoder that rejects an image) costs that step only, and at worst the output is the model as `import` read it: a usable model rather than none. In a container a `SIGKILL` most likely comes from the OOM killer, but nothing confirms it. Only `import` and `export` failing make `optimize` fail, with the step's exit code and error code, and a message naming the step; the workspace goes either way.
- **Interruption.** `SIGINT`, `SIGTERM` and `SIGHUP` go to the running step, which cleans up after itself as any interrupted command does. `optimize` waits for it, unwinds (removing its workspace), then dies of the same signal (`common::deferInterruptions`). A container's `SIGTERM` reaches only `optimize`, which is why it forwards. `test/optimize/optimize_test.cpp` checks this with a stand-in step.

## Plan

`plan()` is a list of conditions and steps, in the step order of [chaining.md](chaining.md#meta-commands):

```cpp
if (model.hasGeometry()) run("normals");
if (model.hasGeometry()) run("weld");
run("dedup");
if (simplifies()) run("simplify", {"--error", number(error)});
if (model.hasTextures() && !fast && texturesWasteSpace(model) && uvFits(model, machine)) run("uv");
if (hasImagesToEncode(model, settings)) run(textureCommand(settings), textureOptions(model, settings));
if (overBudget(model)) run.warn("gpu-budget-exceeded", overBudgetMessage(model));
if (model.hasGeometry()) run("compress");
```

`model` is what the steps so far reported: import's report (primitives and their encodings, vertices, faces, each image's size, format and slots, texture occupancy), then each step's changes to it: the vertices and faces `normals`, `weld` and `simplify` leave, the images `uv`, `ktx` and `avif` write. A condition after a step sees its result, so `uv`'s memory check uses the faces `simplify` left, and the texture side the geometry `simplify` left. Per-model rules go in this list, as a condition on a fact of `Model`; a fact a report doesn't give yet is added to the report, not computed by `optimize`. The rules and their figures come from the corpus survey ([experiments/optimize-survey.md](experiments/optimize-survey.md)); the tree is drawn in [optimize-plan/current.svg](optimize-plan/current.svg).

- **`normals` and `weld` always run on geometry.** Each copies what it doesn't need to change, for a scan of the normals or of the vertices: 0.03 s on a smooth 850,000-vertex scan. Neither decompresses Draco or meshopt primitives, unless `normals` must add normals to them.
- **`dedup` always runs**: nothing to merge costs 0.27 s on the 44-million-face scan ([dedup.md](dedup.md#results)).
- **`simplify`'s error and the image size come from the quality**, after eCorpus's `getPreset` (`optimizeGlb.ts`):

  | Quality | Highest | High (default) | Medium | Low | Thumb |
  | --- | --- | --- | --- | --- | --- |
  | `simplify --error` | not run | 0.0001 | 0.001 | 0.005 | 0.01 |
  | `ktx`/`avif --max-size`, at most | 8192 | 4096 | 2048 | 1024 | 512 |
  | `ktx`/`avif --quality` | Highest | High | Medium | Low | Thumb |

  eCorpus also passes glTF-Transform a `ratio`, min(preset ratio, preset ratio × 1 M / vertices), which works as a floor: simplification stops at that ratio or at the error, whichever comes first. High and Highest (ratio 1) therefore keep at least about 1 M vertices, and every face of a smaller model. `simplify` is error-only ([simplify.md](simplify.md)), so High simplifies smaller models too, within a ten-thousandth of the scene's size. There is no face budget: geometry is cheap next to textures (20 MB of GPU memory for 1 M faces), and a model that stays too heavy at the quality's error is one the plan can't fix without damage it shouldn't do.
- **Compressed geometry is kept**: when every primitive is Draco or meshopt (after `normals`), and the geometry fits [the GPU budget](#gpu-budget), `simplify` doesn't run. It would write the primitives uncompressed, and `compress`'s meshopt is often larger than their Draco: `statue-c` grew from 1.8 to 3.4 MB.
- **`uv` runs when it can gain and fit**: import measured the texture occupancy, it's under 0.6, and `uv`'s xatlas data (65 bytes per face, [uv.md](uv.md#time-and-memory)) fits in half the memory available. `uv` then decides per group whether repacking gains enough. In the survey it repacked no model above 0.6 but cards of a few faces, and spent up to 18 s a model finding that out.
- **Textures:** `ktx` in slow, since KTX2 stays compressed on the GPU, and `avif` in fast ([presets](#presets)): the preset decides, not an option of its own. Both get `--min-size 512`: images of 512 and less on their longest side stay as they are, since their GPU memory is negligible (1.4 MB at most) and encoding gains little. Unless there are hundreds: when the model doesn't fit [the GPU budget](#gpu-budget) at its floor with them left as they are, `ktx` gets `--min-size 0` and encodes them too (the campus scan's 634 images, most of 512 and less: 311 MB in RGBA8). `avif` doesn't, since AVIF uploads as RGBA8 like its sources. The step runs only if some image is to be encoded and not KTX2 already. Its `--max-size` is the quality's, lowered for [the GPU budget](#gpu-budget) and, in fast, for [time](#presets). WebP and AVIF sources are decoded and re-encoded like PNG and JPEG ([images.md](images.md#formats)), so that they get to KTX2.
- **`compress` always runs on geometry**, last. It keeps primitives that are compressed already.

## GPU budget

Textures and geometry should fit 256 MB of GPU memory together, best effort. The estimate, for viewers as three.js loads models:

- **Geometry** once compressed: 16 bytes a vertex (quantized position, normal and texture coordinates) and 12 bytes a face (32-bit indices).
- **Textures** with their mipmaps (× 4/3): PNG, JPEG, WebP and AVIF upload as RGBA8, 4 bytes a texel; KTX2 transcodes ETC1S to BC1 or ETC2 (0.5 byte a texel, 1 with alpha), UASTC to BC7 or ASTC 4×4 (1 byte). Before `ktx` runs, color images count as ETC1S and the others as UASTC, as `ktx` will encode them.

When the textures at the quality's size don't fit beside the geometry, the texture step's `--max-size` goes down by 256 until they do, for every image at once: a material's color and normal maps keep the same size, and halving them all overshoots (each halving divides their memory by 4). It goes no lower than 1024. A model still over budget then, because of its geometry or of many textures, is written as it is, with a warning (`gpu-budget-exceeded`): nothing in the plan cuts faces for memory, and textures under 1024 would be damage for a model that is too heavy anyway. On the survey's corpus, at High, the budget changes two models: the 44-million-face scan's textures go to 3328 (299 MB of textures at 4096; 245 MB in all at 3328), and the campus scan's 634 small images are encoded (311 → 59 MB, [Plan](#plan)).

## Guard

When the exported model would be larger than the input and save less than a tenth of its estimated GPU memory, `optimize` writes import's stage instead (`export --stage 0`), with a warning (`input-kept`): the input as it was, in the normalized profile. That happens to inputs that were optimized already, where the steps can only rewrite what was compressed well. The comparison uses the last stage's files, as the last step that succeeded reported them, against the input's.

## Presets

- **slow**, the default: the asset to publish.
- **fast**: a temporary model, made while the final asset is being worked on, which must come in seconds to be of any use: a mostly-ok output for normal models, degrading as inputs get worse, rather than a run that takes a minute. It skips `uv`, which bakes on one core, and encodes AVIF at effort 2 rather than KTX2: `ktx` has no fast mode, and AVIF at effort 2 is the effort sweep's fastest effort that stays mostly good ([webp-avif.md](experiments/webp-avif.md)).

  AVIF is planned to take about 6 s, from its cost fitted on the survey (4 cores): 0.13 s per output megatexel at effort 2, so the texture side goes down by 256 until the estimate fits, to 512 at the lowest. An encoding that takes longer than planned goes on: the estimate decides only the side. The geometry steps aren't bounded. `simplify` takes about 1.4 s per million faces, a minute on the 44-million-face scan, but a model left with all its faces can be too heavy to look at, and a model one can't look at is the one outcome fast must avoid: a usable model in twice the time is better. `import`, `normals` and `weld` take another 22 s there.

## Memory

`machine.memory` is the lower of the system's available memory (`MemAvailable`) and what the process's cgroup v2 may still use (`memory.max` − `memory.current`), so a container's limit counts. Only `uv`'s condition reads it today. A step that runs out of memory anyway is [left out](#how-it-runs).

## Results

Release build, 4 cores, `--quality High`, on the local disk. Faces are after `simplify`; `ktx` and `avif` take most of the time.

| Model | Faces | slow (KTX2) | fast (AVIF effort 2) |
| --- | --- | --- | --- |
| DamagedHelmet | 15,452 → 15,188 | 16.4 s, 3.77 → 9.75 MB | 2.3 s, 1.49 MB |
| `collection-01` (one 4k image) | 1.66 M → 438,000 | 20.5 s, 51.6 → 4.90 MB | 5.5 s, 3.73 MB |
| `collection-27` (three 4k images) | 200,000 → 183,000 | 46.0 s, 21.2 → 30.0 MB | 6.8 s, 5.59 MB |
| `collection-32` (634 primitives, flat normals) | 672,000 → 593,000 | 70.9 s, 90.1 → 28.7 MB | 15.4 s, 19.2 MB |
| `collection-23` (three 8k images) | 200,000 → 189,000 | 86.4 s, 40.1 → 22.0 MB | 8.6 s, 3.57 MB |

- **fast is 3.5 to 10 times faster.** ETC1S encoding is most of slow's time; `uv` adds 2 to 39 s.
- **KTX2 outputs can be larger than their JPEG inputs** (the helmet, collection-27): UASTC normal and ORM maps are several times their JPEG, the price of staying compressed on the GPU ([ktx.md](ktx.md)).
- **Renders** (`tools/viewer/compare.py`) of collection-01, collection-23 and the campus show slow and fast as their inputs. ChronographWatch at High keeps 85,000 of 100,000 faces, and its strap shades in faint facets, where `simplify`'s own 0.001 default makes them plain ([validation.md](validation.md#field-notes)).
