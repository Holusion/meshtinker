# Experiment: models with too many texels

Some models carry far more texture than a viewer can show, spread over dozens or hundreds of images. This experiment decides what `optimize` should do with them:

- whether the GPU budget alone ([optimize.md](../optimize.md#gpu-budget)) shrinks them enough, or whether a cap on texel density should shrink them further;
- whether merging many images into a few atlases is worth a command;
- how a model that was compressed already, but carelessly, gets past the rule that keeps compressed textures as they are ([images.md](../images.md#formats), "KTX2 is the end of the line").

Its answer becomes conditions in `src/commands/optimize/plan.cpp`, with figures a reader can check against the renders.

## The three models

Selected by `node tools/corpus.mjs paths --covers many-textures` (local models, read in place). They were chosen as three points on one axis: how many of their texels are worth keeping.

| | `local/man80287` | `local/sorcier-raw` | `local/campus-normalized` |
| --- | --- | --- | --- |
| What it is | An object, seen whole and zoomed into | A cave, explored from inside | Buildings from a city-scale scan |
| The user's judgement | Should have far fewer texels; the limit is hard to place | Legitimately needs many texels | Mostly garbage: could fit one or two textures |
| File | 235 MB GLB, meshopt and quantized | 427 MB GLB | 90 MB glTF, 634 JPEG files |
| Faces | 500,000 (no normals) | 355,000 | 672,000 (flat normals) |
| Primitives, one material and one image each | 103 | 56 | 634 |
| Images | 103 × 4096², ETC1S KTX2 | 56 × 4096², JPEG | 289 × 256², 311 × 256×512, 33 × 512², 1 × 512×1024, JPEG |
| Texels, occupancy | 1,728 M, 0.47 | 940 M, 0.66 | 69 M, 0.50 |
| Texels in use (as 4k pages) | 815 M (48.6) | 622 M (37.1) | 35 M (2.1) |
| Surface area | 20.9 m² | 600 m² | 678 m² |
| Texel size on the surface | 0.16 mm | 0.98 mm | 4.4 mm |
| Bounding diagonal | 4.81 m | 25.9 m | 26.2 m |
| Headroom at 4k (below) | 7.3 | 6.4 | 1.4 |
| GPU memory as it is | 1,099 MiB (ETC1S) | 4,779 MiB (RGBA8); 597 MiB once ETC1S | 350 MiB (RGBA8) |
| `optimize` at High today | Nothing done: `input-kept` | Not run; the plan predicts `ktx --max-size 2560` (budget), no `uv` (0.66) | 70.9 s, 90 → 29 MB, every image encoded (59 MB of GPU memory) |

- **Headroom at 4k** is how far one can zoom into the model, from a view where its bounding diagonal spans 4096 pixels, before a texel covers more than a pixel: √(texels in use / surface area) × diagonal / 4096. It is scale-free and comparable across models, but it assumes the model is seen from outside: a cave's visitor stands a metre or two from a wall, 10 to 25 times closer than its diagonal. MAN80287 and the cave score alike (7.3 and 6.4), so **headroom alone doesn't separate them**. What does is the viewing mode, and, in the pilot below, how much detail their texels actually hold.
- **Surface area** comes from Blender (`tools/experiments/many-textures/area.py`), node transforms applied. Blender 5.2 refuses `KHR_texture_basisu`, so MAN80287 was measured on a copy without its textures, made by `import`, `normals` and `export` to a `.gltf` and an edit of its JSON. `inspect` reports no area today.
- **MAN80287 today.** `ktx` keeps its KTX2 images, and `uv` refuses them (`image-already-compressed`, 103 times). `normals` adds normals (the input has none), which decompresses the geometry, so `simplify` runs (500,000 → 450,000 faces), and `compress` warns `repeated-quantization`. The plan warns `gpu-budget-exceeded`, and the guard writes the input back (`input-kept`). Its GPU estimate, 2,208 MB, is twice the real figure: the plan counts any KTX2 source at 1 byte a texel, as if it were UASTC, and these images are ETC1S (BasisLZ supercompression in the KTX2 header).
- **With the budget alone,** if KTX2 could be re-encoded, the plan would give MAN80287 `--max-size 1792`: 220 MiB of textures, 0.37 mm texels, headroom 3.2, and about 45 MB of KTX2 (by texels, from 231 MB). The cave would get 2560: 1.57 mm texels, headroom 4.0.

## Pilot: how much detail the texels hold

Three images per model, shrunk by 2 and 4 (Lanczos 3), enlarged back, and compared with the original (`tools/experiments/many-textures/downup.sh`; MAN80287's images decoded by `ktx extract --transcode rgba8`). High figures mean the image holds little detail at its full size.

| Image | ÷2: PSNR, SSIM | ÷4: PSNR, SSIM |
| --- | --- | --- |
| MAN80287 0 (occupancy 0.38) | 33.7 dB, 0.92 | 31.4 dB, 0.85 |
| MAN80287 18 (0.76) | 29.9 dB, 0.83 | 27.6 dB, 0.68 |
| MAN80287 50 (0.54) | 33.2 dB, 0.91 | 30.8 dB, 0.84 |
| Sorcier 3 | 25.8 dB, 0.68 | 24.4 dB, 0.54 |
| Sorcier 20 | 24.5 dB, 0.71 | 23.1 dB, 0.56 |
| Sorcier 40 | 26.0 dB, 0.75 | 24.6 dB, 0.62 |
| Campus 319 (256×512) | 23.1 dB, 0.83 | 20.5 dB, 0.63 |
| Campus 436 (256×512) | 23.8 dB, 0.82 | 21.3 dB, 0.61 |
| Campus 317 (256×512) | 24.2 dB, 0.79 | 21.9 dB, 0.57 |

- MAN80287's textures look soft at 1:1: grain over a few texels, no sharp edges. The cave's are sharp, with fine cracks and gravel. In both, the images looked at are four 2k pages each, separated by black lines.
- **Biases, to remove before trusting the figures:** the whole image is compared, padding included. MAN80287's padding is a blurred dilation, which raises its figures. MAN80287's source is ETC1S, whose 4×4 block artefacts count as detail and lower them. Mask the comparison to the texels that the scene samples (the occupancy map), and compare MAN80287 at a block-aligned factor.
- If the masked figures hold, **the detail a texture actually holds** could separate an oversampled object from a dense cave without knowing how either is viewed. That would be a fact a report can give: per image, the largest shrink factor whose loss stays under a threshold.

## Questions

1. **Re-encoding compressed textures.** Proposed rule: a KTX2 image is decoded and encoded again when the texture step's side is smaller than the image, which only happens when the budget (or the quality's cap) shrinks it. This accepts a second lossy round, recorded as `repeated-lossy-image-encoding`, against a fivefold memory gain. Does the second round show in renders, beside the loss from shrinking? Is ETC1S, decoded and encoded again, larger than ETC1S from a JPEG at the same size? Geometry has the same rule already: compressed geometry is simplified when it doesn't fit the budget (`keepsCompression` in `plan.cpp`), and MAN80287's was decompressed anyway by `normals`, since it had none.
2. **Where to stop.** For each model, image quality against texels kept, at several viewing distances. Is there a knee, and does it fall at the same headroom on MAN80287 and on the campus? Does the budget's 1792 for MAN80287 sit before or after it?
3. **Viewing mode.** Does the cave lose visibly at the side that the knee on the objects would give it, from inside? If so, the rule needs a fact that tells caves and rooms from objects: a scene camera inside the bounds (eCorpus's scene setup), an option given by eCorpus, or texture detail as measured in the pilot.
4. **Atlases.** Does merging images into a few repacked pages pay: for the campus, 634 images into one or two 4k pages (35 M texels in use, 2.1 pages at full size); for MAN80287, 103 pages at 0.47 occupancy into fewer and fuller ones? Gains to measure: GPU memory at the same texel density, file size, draw calls (one per primitive and material today) and load time in the viewer.
5. **Detail as a fact.** Does the masked pilot measure (shrink factor under a loss threshold) predict the knee of question 2, image by image? If it does, it can set each image's side without a density rule.

## Treatments

Each treatment is a chain run by hand with the existing commands, unless noted. Quality High, slow preset.

| Name | What | MAN80287 | Sorcier | Campus |
| --- | --- | --- | --- | --- |
| T0 | The input | yes | yes | yes |
| T1 | `optimize` today, plus KTX2 re-encoded at the budget's side (needs prerequisite 1) | 1792 | 2560 | as today |
| T2 | A sweep of the texture side past the budget: `ktx --max-size` 3072, 2048, 1536, 1024, 768, 512 | yes | yes | sides scaled to the images: ÷1, ÷2, ÷4 |
| T3 | `uv` before T1, KTX2 decoded | yes (0.47) | yes, forced (0.66) | yes (0.50) |
| T4 | Atlases: every chart of every image packed into the fewest pages of 4096², then T2's sweep on the pages | yes | no | yes |

- **T2 is the core measurement:** its rows give question 2's curve. Record each step's headroom, so that the curves of the three models share an axis.
- **T4 has no command yet.** It is `uv` across materials: materials that differ only by their base color image (all three models) become one, whose charts xatlas packs together, as `uv` packs one material's charts today ([uv.md](../uv.md)). For the experiment, a Blender prototype is enough: pack every object's UV islands into one shared layout and bake the base colors into it. Build the command only if T4 wins.

## Measurements

Per output: bytes, estimated GPU memory, texels, headroom, primitives and materials (draw calls), and each step's time.

Quality, with renders (`tools/viewer/`, three.js set up as Voyager), each output against T0's render of the same view, scored by SSIMULACRA2 (`ssimulacra2_rs`, as in [webp-avif.md](webp-avif.md)):

- **Full view:** three azimuths at zoom 1. Most viewers see this view only.
- **Close-ups:** zoom 2 and 4 on two detailed points per model, chosen once from T0 and kept for every treatment.
- **The cave from inside:** two views from a point inside it. `view.html` places the camera on a sphere around its target, so it needs an eye position (`eye=x,y,z`) first.
- Render at 1600×1200.

Write the views and the scores to one CSV per model, with paths to the images, as `webp-avif-render-*.csv` does. Keep the renders, in pairs (T0 and output, same view): they are the set a person, or later a vision model, looks at to judge whether a loss shows. SSIMULACRA2 decides first; a vision model's verdict is worth adding once the scores leave a range where the threshold isn't clear, and it can be checked against the person's on the same pairs.

## Prerequisites

1. **KTX2 decoding in `image::decode`**, with basisu's transcoder (vendored for the encoder, used by `test/ktx/ktx_test.cpp`): ETC1S and UASTC to RGBA8. Then `ktx` and `uv` take KTX2 images that the plan asks them to change. Until it exists, T1 and T3 on MAN80287 can run on a copy whose images are decoded by `ktx extract --transcode rgba8` and referenced as PNG, in the experiment's directory.
2. **The plan's GPU estimate for KTX2 sources:** ETC1S at 0.5 byte a texel, UASTC at 1, from the KTX2 header (`supercompressionScheme` BasisLZ is ETC1S), reported by `inspect` per image.
3. **`view.html`'s eye position**, for the cave.
4. **The occupancy map as an image**, to mask the pilot's comparison: `analysis` computes it, nothing writes it out.

## Deliverables

- One section per question in this document, with tables. The CSVs and the render pairs go in the repository only if small (a few MB); otherwise they are listed with the commands that remake them.
- The rule each question settles, as conditions on `Model` facts in `plan.cpp`, and the facts the reports must add for them (surface area or headroom, per-image detail, KTX2 codec).
- Whether T4's atlas command is worth building, as a TODO item with its figures.

## Setup

- `tools/experiments/many-textures/`: `glb_images.py` (images out of a GLB by index), `area.py` (surface area, Blender), `downup.sh` (the pilot).
- Work in `build/exp-texels/` (gitignored, on the local disk): the scratch directory is on tmpfs, and a sweep writes several hundred MB a model.
- The cave's `optimize` at High encodes 56 4k images: expect several minutes a run.
