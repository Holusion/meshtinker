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
| `optimize` at High | `uv --max-size 1792`, `ktx --max-size 2560`: 11.6 min, 235 → 72 MB, 245 MiB of GPU memory | `ktx --max-size 2560`, no `uv` (0.66): 7.6 min, 427 → 80 MB, 243 MiB | `uv`, `ktx --min-size 0`: 74 s, 90 → 29 MB, 59 MiB |

- **Headroom at 4k** is how far one can zoom into the model, from a view where its bounding diagonal spans 4096 pixels, before a texel covers more than a pixel: √(texels in use / surface area) × diagonal / 4096. It is scale-free and comparable across models, but it assumes the model is seen from outside: a cave's visitor stands a metre or two from a wall, 10 to 25 times closer than its diagonal. MAN80287 and the cave score alike (7.3 and 6.4), so **headroom alone doesn't separate them**. What does is the viewing mode, and, in the pilot below, how much detail their texels actually hold.
- **Surface area** comes from Blender (`tools/experiments/many-textures/area.py`), node transforms applied. Blender 5.2 refuses `KHR_texture_basisu`, so MAN80287 was measured on a copy without its textures, made by `import`, `normals` and `export` to a `.gltf` and an edit of its JSON. `inspect` reports no area today.
- **MAN80287 under the plan.** `normals` adds normals (the input has none), which decompresses the geometry, so `simplify` runs (500,000 → 450,000 faces), and `compress` warns `repeated-quantization`. The budget gives the texture step 1792, under the images' 4096, so `uv` gets `--max-size 1792` and decodes the KTX2 images ([images.md](../images.md#formats)): it repacks 85 of the 103 (occupancy 0.47), the other 18 having nothing to gain. After `uv`, the budget is computed again on the smaller pages and gives 2560, at which `ktx` encodes the 85 PNG pages and the 18 KTX2 images left, warning `repeated-lossy-image-encoding` 103 times. The GPU estimate reads each source's codec (ETC1S, 0.5 byte a texel): 1,110 MiB as it is.

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
- **Octave energies** say the same thing more directly, without a threshold (`tools/experiments/many-textures/octaves.sh`). The mean square of each band of the luma's Laplacian pyramid is the contrast in that octave. Natural images are close to scale-invariant (power spectrum in 1/f²), which puts the same energy in every octave. So the ratio of the finest octave to the next, E0/E1, is near 1 for an image with detail down to its texels, well under 1 for a soft or upsampled one, and over 1 for noise or sharpening. A band's kurtosis tells structure (sparse, heavy-tailed) from Gaussian noise (3). Padding scales both bands alike, so it leaves the ratio, but it raises the kurtosis, by 1/occupancy for a noise-only band.

  | Image | E0/E1 | E1/E2 | Kurtosis of E0 |
  | --- | --- | --- | --- |
  | MAN80287 0, 18, 50 | 0.75, 0.79, 0.64 | 1.02, 1.00, 0.70 | 17, 14, 47 |
  | Sorcier 3, 20, 40 | 1.65, 1.56, 1.67 | 1.18, 1.26, 1.41 | 9, 9, 11 |
  | Campus 319, 436, 317 | 0.58, 0.63, 0.67 | 0.70, 0.72, 0.83 | 11, 11, 8 |

  MAN80287 holds detail down to 2k (E1/E2 near 1), and its 4k octave is weak: about one octave of oversampling, despite the ETC1S block artefacts adding to E0. The campus's images are soft at every scale: upsampled or blurred at the source. The cave has more energy at its finest octave than scale invariance gives, with a kurtosis of about 10, too high for Gaussian noise even after padding (3 / 0.66 = 4.5): fine structure, sharpening, or JPEG's 8×8 block edges, which the masked run should tell apart by measuring the band at block boundaries separately. The vips script takes about 2 s per 4k image through temporary files. In C++, after decoding, a pyramid of a 4k luma is a few tens of milliseconds, so the cost is the decode, which the texture step pays anyway.
- If the masked figures hold, **the detail a texture actually holds** could separate an oversampled object from a dense cave without knowing how either is viewed. That would be a fact a report can give: per image, the finest octave whose energy ratio stays near 1, which is the side the image needs.

### Masked

The same nine images, measured on the texels the scene samples only (`masked.sh`, with `inspect --occupancy-maps`'s map of each image; MAN80287's decoded by `ktx extract --transcode rgba8`). PSNR is over RGB after shrinking and enlarging back; the octave energies are of the luma, each band weighted by the mask shrunk to its level; K0 is band 0's kurtosis on the sampled texels. B8 and B4 are band 0's energy on texels next to an 8×8 (JPEG) or 4×4 (ETC1S) block edge, over its energy elsewhere.

| Image | Occupancy | ÷2 PSNR | ÷4 PSNR | E0/E1 | E1/E2 | K0 | B8 | B4 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| MAN80287 0 | 0.38 | 29.7 dB | 27.3 dB | 0.75 | 1.07 | 6.8 | 1.09 | 1.18 |
| MAN80287 18 | 0.77 | 29.2 dB | 26.8 dB | 0.80 | 1.12 | 6.9 | 1.07 | 1.15 |
| MAN80287 50 | 0.54 | 31.5 dB | 29.0 dB | 0.64 | 0.79 | 9.1 | 1.09 | 1.17 |
| Sorcier 3 | 0.82 | 25.0 dB | 23.7 dB | 1.73 | 1.37 | 7.7 | 1.00 | 1.01 |
| Sorcier 20 | 0.73 | 23.3 dB | 21.9 dB | 1.62 | 1.41 | 6.9 | 1.00 | 1.01 |
| Sorcier 40 | 0.64 | 24.1 dB | 22.8 dB | 1.73 | 1.58 | 6.9 | 1.00 | 1.01 |
| Campus 319 | 0.46 | 22.9 dB | 20.0 dB | 0.52 | 0.67 | 9.7 | 1.07 | 1.11 |
| Campus 436 | 0.10 | 21.9 dB | 19.2 dB | 0.60 | 0.73 | 7.8 | 0.97 | 1.01 |
| Campus 317 | 0.63 | 22.5 dB | 20.0 dB | 0.78 | 1.02 | 9.1 | 1.04 | 1.03 |

- **Masking lowers MAN80287's PSNR by 0.8 to 4 dB** (33.7 → 29.7 dB at ÷2 for image 0): its blurred padding made it look softer than it is. The cave and the campus move by 0.2 to 1.9 dB. The ranking holds (MAN80287 loses least), but the gap to the cave shrinks from about 8 dB to about 6.
- **The energy ratios don't move**: MAN80287's E0/E1 is 0.75, 0.80, 0.64 masked, as whole. Padding scales every band alike, as expected, so the ratios need no mask; the kurtosis does (MAN80287's 14 to 47 whole, 7 to 9 masked).
- **ETC1S's blocks show in MAN80287's finest octave** (B4 1.15 to 1.18): its E0 is overstated, which makes it look sharper than it is, not softer. Its weak 4k octave is therefore not an artefact.
- **The cave's excess fine energy isn't JPEG's blocks** (B8 1.00): it is in the texture itself, fine structure or sharpening at capture, with a kurtosis of 7 to 8, too high for Gaussian noise. Either way, it is what a viewer sees close up.
- The campus's images are soft at every octave (E0/E1 0.5 to 0.8), and 436, sampled at 10 %, is mostly padding.

So the pilot's reading stands once masked: MAN80287 has about one octave of oversampling (E0/E1 0.64 to 0.80, E1/E2 near 1), the campus one to two, the cave none. Whether that predicts the knee in renders is question 5 ([below](#question-5-detail-as-a-fact)).

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
- **The cave from inside:** two views from points inside it, with `view.html`'s `eye=x,y,z`.
- Rendered at 1200×900: at 1600×1200, SwiftShader drew nothing once zoomed in on MAN80287's 103 4k textures.

Write the views and the scores to one CSV per model, with paths to the images, as `webp-avif-render-*.csv` does. Keep the renders, in pairs (T0 and output, same view): they are the set a person, or later a vision model, looks at to judge whether a loss shows. SSIMULACRA2 decides first; a vision model's verdict is worth adding once the scores leave a range where the threshold isn't clear, and it can be checked against the person's on the same pairs.

## Results

Run on 2026-11-25 with the commands of this branch, on the machine of [validation.md](../validation.md) (4 cores). `treatments.py MODEL` remakes everything; the outputs, renders and CSVs (`build/exp-texels/MODEL/results.csv`, about 60 MB of renders a model) stay out of the repository.

### How to read the scores

- **The references.** SSIMULACRA2 is taken against three renders, each isolating one change:
  - **B**, the base: the plan's geometry steps (`normals`, `weld`, `dedup`, `simplify`) with the input's images untouched. Against T0, B already scores about 70 at full view and 33 to 61 close up. The geometry moves the surface by a fraction of a texel, and the grain then lands elsewhere; T0 and B look alike side by side. `normals` matters too: MAN80287 has none, and the campus has flat ones, which it makes smooth.
  - **The KTX2 output that keeps every texel:** Q1-4096 for the cave (ETC1S at 4096), T2-512 for the campus (ETC1S at the images' own sizes). A JPEG source and its KTX2 differ at every side, by about 25 points at full view. three.js makes a JPEG's mipmaps on the GPU, and basisu makes KTX2's in linear light, so the far views, which sample low mips, differ in brightness. Against the KTX2 ceiling, the scores show what the side costs.
  - **For MAN80287, B itself:** its sources are ETC1S already, so there is no format offset.
- **Noise floor:** two renders of B score 97.5 against each other (SwiftShader isn't bit-exact).
- **Mip alignment:** a side that isn't the source's divided by a power of two (3072, 1536, 768 of 4096) puts its mip levels at other footprints. Far views score it lower than a smaller side that aligns: the cave's exterior gives 73 for 3072 and 77 for 2048. The zoom-4 views sample the base level and rank the sides in order: they are the measure of texel detail.
- **Columns:** "full" is the mean of the three azimuths, "z2" and "z4" the mean of the two close-up points at zoom 2 and 4. For the cave, "inside" and "inside z4" are the two eyes at a 60° and a 15° field of view.
- **Bytes and time:** T2's textures are measured alone, without `compress` (T2 outputs keep their geometry uncompressed). "Time" is the texture step's (the whole `optimize` for T1-opt).

| MAN80287, against B | Textures MB | GPU MiB | Texels in use (M) | Headroom | Time (s) | full | z2 | z4 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T0, B | 231 | 1110 | 815 | 7.34 | — | — | — | — |
| T2-3072 | 135 | 628 | 461 | 5.52 | 762 | 84 | 76 | 71 |
| T2-2048 | 65 | 285 | 207 | 3.70 | 329 | 77 | 66 | 57 |
| T2-1536 | 39 | 165 | 118 | 2.79 | 201 | 71 | 58 | 46 |
| T2-1024 | 20 | 79 | 53 | 1.88 | 110 | 66 | 49 | 30 |
| T2-768 | 13 | 49 | 31 | 1.42 | 38 | 62 | 44 | 19 |
| T2-512 | 8 | 28 | 14 | 0.96 | 21 | 58 | 37 | 4 |
| T1, no `uv`: `ktx` 1792 | 51 | 221 | 159 | 3.24 | 260 | 72 | 59 | 47 |
| T3: `uv` 1792, `ktx` 1792 | 58 | 207 | 194 | 3.57 | 152 + 283 | 72 | 62 | 52 |
| `optimize`: `uv` 1792, `ktx` 2560 | 67 | 245 | 229 | 3.89 | 696 | 71 | 60 | 49 |

| The cave, against Q1-4096 | Textures MB | GPU MiB | Texels in use (M) | Headroom | Time (s) | full | inside | inside z4 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T0, B | 412 | 4789 | 622 | 6.44 | — | — | — | — |
| Q1-4096: ETC1S, all texels | 189 | 607 | 622 | 6.44 | 1093 | — | — | — |
| T2-3072 | 108 | 346 | 353 | 4.85 | 575 | 73 | 42 | 33 |
| T2-2048 | 50 | 159 | 160 | 3.27 | 265 | 77 | 49 | 24 |
| T2-1536 | 30 | 94 | 92 | 2.48 | 164 | 73 | 40 | 9 |
| T2-1024 | 15 | 47 | 42 | 1.68 | 85 | 74 | 36 | −10 |
| T2-768 | 9 | 31 | 24 | 1.28 | 28 | 68 | 25 | −27 |
| T2-512 | 5 | 19 | 11 | 0.87 | 16 | 60 | 11 | −45 |
| T3: `uv` forced, `ktx` 2560 | 77 | 242 | 256 | 4.13 | 403 | 70 | 41 | 28 |
| Q1-2048: Q1-4096 encoded again at 2048 | 50 | 159 | 160 | 3.27 | 256 | 77 | 46 | 24 |
| `optimize` (T1): `ktx` 2560 | 76 | 243 | 247 | 4.06 | 455 | 72 | 39 | 20 |

| The campus, against T2-512 | Textures MB | GPU MiB | Texels in use (M) | Headroom | Time (s) | full | z2 | z4 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T0 | 22 | 388 | 34 | 1.44 | — | — | — | — |
| T2-512: ETC1S at the images' sizes | 20 | 66 | 34 | 1.44 | 75 | — | — | — |
| T2-256 | 9 | 43 | 14 | 0.91 | 26 | 78 | 41 | 24 |
| T2-128 | 3 | 28 | 4 | 0.48 | 9 | 60 | 8 | −16 |
| `optimize` (T1 = T3): `uv`, `ktx` | 19 | 59 | 35 | 1.46 | 74 | 77 | 69 | 67 |
| T4, the input's images in 6 pages of 3840 | 13 | 94 | 35 | 1.46 | 97 | 74 | 58 | 47 |
| T4 ÷2 (1920) | 4 | 52 | 10 | 0.78 | 33 | 70 | 33 | 10 |
| T4 ÷4 (960) | 1 | 42 | 3 | 0.41 | 8 | 55 | 0 | −27 |
| T4 after `uv`: 5 pages of 3840 | 12 | 70 | 35 | 1.46 | 80 | 72 | 58 | 47 |
| T4 after `uv` ÷2 (1920) | 3 | 34 | 10 | 0.78 | 27 | 69 | 33 | 11 |
| T4 after `uv` ÷4 (960) | 1 | 26 | 3 | 0.41 | 7 | 56 | 1 | −27 |

The scores against T0 and against B are in each `results.csv`, for every view; `summary.py MODEL[:REFERENCE]` prints the tables. The render pairs to look at are in `build/exp-texels/MODEL/renders/`. Looked at by eye:

- MAN80287 at zoom 4: 2048 is barely softer than B, and 1024 visibly blurred. At full view, 1024 and 512 lose only the sand grain's sparkle.
- The cave from inside, at 60°: B, 2048 and 1024 look alike. At 15°, 2048 is slightly soft on the near floor and 1024 clearly blurred.
- The campus at zoom 4: `optimize`'s output, T2-512 and the atlas after `uv` look alike; the atlas is barely smoother on the trees.

### Question 1: re-encoding compressed textures

**Answer: the second round costs less than one step of the sweep, and the rule stands as applied.**

- **Measured on the cave,** whose JPEG sources allow the comparison: ETC1S at 4096 stands for a careless first compression (Q1-4096), and Q1-2048 encodes it again at 2048. It is set against T2-2048, encoded once from the JPEG.
- **Bytes:** 50.39 MB against 50.19 MB (+0.4 %).
- **On the images** (3, 20 and 40, against the JPEG shrunk to 2048): 47 to 49 against 51 to 54, a loss of 4 to 5 points and 0.4 dB of PSNR. The first round, at 4096, scores 43 to 45 against its JPEG: the first encoding costs more than the second.
- **In renders:** equal at full view (77 and 77), then 46 against 49 inside and 24 against 24 at 15°. The gap is smaller than one step of the sweep (2048 → 1536 is 49 → 40 inside).
- **MAN80287** has no source to compare with, but every one of its outputs went through the second round. Its T2-3072 scores 84, 76 and 71 against its own ETC1S images, and the side, not the round, drives the rest of its curve.

So re-encoding a KTX2 image that the plan shrinks, for a fivefold memory gain on MAN80287 (1,110 → 207 to 245 MiB), is worth its second round. `repeated-lossy-image-encoding` records it.

### Question 2: where to stop

**Answer: there is no knee. Quality falls steadily with the texels kept, at the same rate on both objects, and the budget's sides sit where losses start to show at zoom 4.**

- **A steady fall,** as headroom goes down by steps of about 1.4:
  - MAN80287 loses about 5 points a step at full view, 8 at zoom 2 and 13 at zoom 4.
  - The campus, already at headroom 1.44, loses 28 points at zoom 2 for its first halving.
- **The ranges that hold:**
  - Zoom 4, by eye, holds down to a headroom of about 3.5: MAN80287's 2048 (3.70) is barely softer than B, and the cave's 2048 (3.27) slightly soft at 15°.
  - Full views hold to about 1.9: MAN80287 at 1024.
  - From 3072 to 2048, MAN80287 loses 14 points at zoom 4 and the cave 9 at 15°: no model falls off a cliff where another holds.
- **The budget's 1792 for MAN80287** (headroom 3.2 without `uv`, 3.6 with it) sits at the start of the zoom-4 losses: 47 to 52 at z4, against 57 for 2048, and 72 at full view. It falls before the steep part, which starts under 1536 (headroom 2.8).
- **`uv` before `ktx` (T3)** keeps more texels for less memory: 194 M against 159 M in use, 207 against 221 MiB. It scores 0 to 5 points higher, at 2.6 min of `uv`.
- **No density cap is added:**
  - The budget alone brings the three models to headrooms of 3.6 to 4.1, or keeps them where they are (the campus, 1.44).
  - A cap at a headroom of about 3.5 would change only the cave (2560 → about 2200, 243 → about 180 MiB), where the losses from inside already show at 15°.
  - TODO.md's pending decision keeps the figures, for a model that the budget lets through at full size and that turns out to need a cap.

### Question 3: viewing mode

**Answer: from inside, the cave at the budget's side looks like B at a normal field of view, so no fact about viewing mode is needed for the budget alone.**

- **At 60°,** B, 2048 (headroom 3.3) and 1024 (1.7) look alike, though the scores fall: 49 and 36 against the KTX2 ceiling.
- **At 15°,** like looking through binoculars, 2048 softens the near floor and 1024 blurs it. A visitor at a normal field of view stands where the zoom-2 views of an object stand.
- **The headroom of a cave** is computed from its outside diagonal, so the same headroom means closer views from inside. The budget's 2560 (headroom 4.06) keeps a step's margin.
- **A density cap would need the fact.** A cap set from the objects (about 3.5) would cut the cave to where 15° views blur. eCorpus's scene setup (a camera inside the bounds) is the likely source of the fact.

### Question 4: atlases

**Answer: atlases pay only in draw calls. The campus goes from 634 primitives to 5, with smaller files but more GPU memory, and 10 points lower close up. Building the command waits for a measure of draw calls on a phone.**

- **The prototype is not Blender.** Blender's island packer (`atlas.py`) fails on the campus, whose UVs are one island per triangle: it scales them to 3.6 % of the page. `atlas_rects.py` packs each image whole instead, with 4 texels of its own edges as padding. It uses pages of at most 4096, since phones often go no further and SwiftShader drew nothing for one 9216 page, and gives one material and one merged primitive per page.
- **The campus, from `uv`'s images:**
  - Pages: 5 of 3840, 78 % of their texels image texels, against 634 images and 634 draws.
  - Textures: 12 MB against 19 MB for `optimize` (−37 %). One large ETC1S file compresses better than 634 small ones with their mips.
  - GPU memory: 70 MiB against 59 (+19 %), from the pages' empty space.
  - Quality: 72, 58 and 47 against 77, 69 and 67 at the same density. The difference barely shows by eye, slightly smoother trees. The likely causes: one ETC1S codebook shared by a whole page instead of one per image, and mip levels reading neighbours past the 4 texels of padding. Neither is measured apart.
  - Loading: no difference in the headless viewer (27 against 28 s to the first frame), where geometry dominates; draw calls cost little on SwiftShader and much on phones.
- **MAN80287, estimated:** after `uv`, its 309 M texels fill about 21 pages of 4096 at 78 %, so 103 draws become 21, for the same memory. The 103 images already are pages.
- **An atlas never lowers the texels in use.** `uv` does that, and it already runs per material.

### Question 5: detail as a fact

**Answer: not shown. The octave ratios order the models, but the losses fall steadily on every model, so no image gives a side at which to stop.**

- **What the ratios predict:**
  - MAN80287 (E0/E1 0.64 to 0.80): renders that lose least per texel removed.
  - The cave (1.6 to 1.7): renders that lose most from inside.
  - The campus (0.5 to 0.8): renders that lose most once the images are shrunk, since they start at a headroom of 1.44.
- **Why no side follows:** the scores show no knee to predict (question 2), and the views of the three models differ too much to compare their losses at one headroom.
- **What is left:** the measure may still order images within a model, for a budget that would shrink the softest images first. That needs per-image renders, which this experiment didn't do.

### Rules and follow-ups

- **Kept:** the re-encoding rule as applied (question 1), and the budget as the only rule for size (questions 2 and 3). `plan.cpp` is unchanged beyond the rule.
- **New TODO items** (TODO.md): `uv`'s side after repacking (MAN80287's pages are baked at 1792, then encoded at 2560 beside the 18 images `uv` left); an atlas command, once a phone measures draw calls.
- **Pending decisions** (TODO.md): no density cap; atlases deferred.

## Deliverables

- One section per question in this document, with tables. The CSVs and the render pairs go in the repository only if small (a few MB); otherwise they are listed with the commands that remake them.
- The rule each question settles, as conditions on `Model` facts in `plan.cpp`, and the facts the reports must add for them (surface area or headroom, per-image detail, KTX2 codec).
- Whether T4's atlas command is worth building, as a TODO item with its figures.

## Setup

- `tools/experiments/many-textures/`: `glb_images.py` (images out of a GLB by index), `area.py` (surface area, Blender), `downup.sh` and `octaves.sh` (the pilot), `masked.sh` (the pilot on the sampled texels, with `inspect --occupancy-maps`), `treatments.py` (runs, measures, renders and scores the treatments of one model; `views.json` holds the views), `summary.py` (the tables below, against B or another treatment), `atlas_rects.py` (T4's atlases) and `atlas.py` (Blender's island packer, which fails on the campus: [Question 4](#question-4-atlases)).
- `tools/viewer/serve.py raw=... normalized=... exp=build/exp-texels` must be running for the renders; `screenshot.mjs --view` draws every view of a model from one load.
- Work in `build/exp-texels/` (gitignored, on the local disk): the scratch directory is on tmpfs, and a sweep writes several hundred MB a model.
- The cave's `optimize` at High encodes 56 4k images: expect several minutes a run.
