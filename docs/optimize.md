# optimize

How `meshtinker optimize` turns a model into an optimized one in one step, and why. The code is `src/commands/optimize/`: `plan.cpp` decides which steps run, and everything else runs them.

```bash
meshtinker optimize in.glb out.glb                        # slow preset, quality High, KTX2
meshtinker optimize in.gltf out.glb --quality Low --textures avif
meshtinker optimize in.glb out.glb --preset fast          # a temporary model, quickly
```

## How it runs

`optimize` is a meta-command ([chaining.md](chaining.md#meta-commands)): it runs the other commands as child processes on a chain of its own, as a user would by hand.

1. It creates its workspace beside the output (or in `--tmp-dir`), and runs `import in <workspace>/chain`.
2. It runs the plan's steps on the chain, each with `--json`, reading each report before deciding the next step.
3. It runs `export <workspace>/chain out`, which writes a `.glb` or `.gltf` by the output's extension.
4. It removes the workspace, chain included, unless `--keep-tmp` is given: the chain then stays for debugging, each stage with its `stage.json`.

- **Finding the commands.** `meshtinker-<name>` next to `meshtinker-optimize` first, then on `PATH`: the wrapper's lookup, in `common` (`common/subcommands.hpp`).
- **Output.** Each step's stderr passes through, so warnings appear once, from the command that raised them, as it runs. The text report lists the steps with their times, then the bytes before and after. With `--json`: `settings`, `machine.memory`, `steps` (`command`, `argv`, `exitCode`, `seconds`, and the step's own `report`), `input.byteSize`, `output.byteSize`, `seconds`, and `diagnostics`, every step's tagged with its `step`.
- **Failures.** A step that fails makes `optimize` fail with the step's exit code and error code; its message names the step. A step killed by a signal is an internal error (70): in a container a `SIGKILL` most likely comes from the OOM killer, but nothing confirms it. The workspace goes either way.
- **Interruption.** `SIGINT`, `SIGTERM` and `SIGHUP` go to the running step, which cleans up after itself as any interrupted command does. `optimize` waits for it, unwinds (removing its workspace), then dies of the same signal (`common::deferInterruptions`). A container's `SIGTERM` reaches only `optimize`, which is why it forwards. `test/optimize/optimize_test.cpp` checks this with a stand-in step.

## Plan

`plan()` is a list of conditions and steps, in the step order of [chaining.md](chaining.md#meta-commands):

```cpp
if (model.hasGeometry()) run("normals");
if (model.hasGeometry()) run("weld");
run("dedup");
if (model.hasGeometry() && error > 0) run("simplify", {"--error", number(error)});
if (model.hasTextures() && !fast && texturesWasteSpace(model) && uvFits(model, machine)) run("uv");
if (model.hasTextures()) run(textureCommand(settings), textureOptions(settings));
if (model.hasGeometry()) run("compress");
```

`model` is what the steps so far reported: import's report (faces, images, texture occupancy), then simplify's face count. A condition after a step sees its result, so `uv`'s memory check uses the faces `simplify` left. Per-model rules go in this list, as a condition on a fact of `Model`; a fact a report doesn't give yet is added to the report, not computed by `optimize`.

- **`normals` and `weld` always run on geometry.** Each copies what it doesn't need to change, for a scan of the normals or of the vertices: 0.03 s on a smooth 850,000-vertex scan.
- **`dedup` always runs**: nothing to merge costs 0.27 s on the 44-million-face scan ([dedup.md](dedup.md#results)).
- **`simplify`'s error and the image size come from the quality**, after eCorpus's `getPreset` (`optimizeGlb.ts`):

  | Quality | Highest | High (default) | Medium | Low | Thumb |
  | --- | --- | --- | --- | --- | --- |
  | `simplify --error` | not run | 0.0001 | 0.001 | 0.005 | 0.01 |
  | `ktx`/`avif --max-size` | 8192 | 4096 | 2048 | 1024 | 512 |
  | `ktx`/`avif --quality` | Highest | High | Medium | Low | Thumb |

  eCorpus also passes glTF-Transform a `ratio` that works as a floor: High and Highest keep every face. `simplify` is error-only ([simplify.md](simplify.md)), so High simplifies, within a ten-thousandth of the scene's size.
- **`uv` runs when it can gain and fit**: import measured the texture occupancy, it's under 0.9, and `uv`'s xatlas data (65 bytes per face, [uv.md](uv.md#time-and-memory)) fits in half the memory available. `uv` then decides per group whether repacking gains enough.
- **Textures:** `ktx` by default, which stays compressed on the GPU; `avif` with `--textures avif`, smaller to download ([avif.md](avif.md)).
- **`compress` always runs on geometry**, last.

## Presets

- **slow**, the default: the asset to publish.
- **fast**: a temporary model, made while the final asset is being worked on, which must come about an order of magnitude faster to be of any use. It skips `uv`, which bakes on one core, and encodes AVIF at effort 2, whatever `--textures` says: `ktx` has no fast mode, and AVIF at effort 2 is the effort sweep's fastest effort that stays mostly good ([webp-avif.md](experiments/webp-avif.md)). On the eight models of the viewer pass, `ktx` took 502 s in total and `avif` at effort 3 69 s. The fast textures experiment ([fast-textures.md](experiments/fast-textures.md)) may change this.

## Memory

`machine.memory` is the lower of the system's available memory (`MemAvailable`) and what the process's cgroup v2 may still use (`memory.max` − `memory.current`), so a container's limit counts. Only `uv`'s condition reads it today.

## Results

Release build, 4 cores, `--quality High`, on the local disk. Faces are after `simplify`; `ktx` and `avif` take most of the time.

| Model | Faces | slow (KTX2) | fast (AVIF effort 2) |
| --- | --- | --- | --- |
| DamagedHelmet | 15,452 → 15,188 | 16.4 s, 3.77 → 9.75 MB | 2.3 s, 1.49 MB |
| `01-akhenaton-nefertiti` (one 4k image) | 1.66 M → 438,000 | 20.5 s, 51.6 → 4.90 MB | 5.5 s, 3.73 MB |
| `27-athlete-clean-01` (three 4k images) | 200,000 → 183,000 | 46.0 s, 21.2 → 30.0 MB | 6.8 s, 5.59 MB |
| `32-campus-romainville` (634 primitives, flat normals) | 672,000 → 593,000 | 70.9 s, 90.1 → 28.7 MB | 15.4 s, 19.2 MB |
| `23-man-10918-pyxide` (three 8k images) | 200,000 → 189,000 | 86.4 s, 40.1 → 22.0 MB | 8.6 s, 3.57 MB |

- **fast is 3.5 to 10 times faster.** ETC1S encoding is most of slow's time; `uv` adds 2 to 39 s.
- **KTX2 outputs can be larger than their JPEG inputs** (the helmet, the athlete): UASTC normal and ORM maps are several times their JPEG, the price of staying compressed on the GPU ([ktx.md](ktx.md)).
- **Renders** (`tools/viewer/compare.py`) of akhenaton, pyxide and the campus show slow and fast as their inputs. ChronographWatch at High keeps 85,000 of 100,000 faces, and its strap shades in faint facets, where `simplify`'s own 0.001 default makes them plain ([validation.md](validation.md#field-notes)).
