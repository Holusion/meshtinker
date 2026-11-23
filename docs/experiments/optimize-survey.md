# Experiment: optimize's plan against the corpus

How `optimize`'s plan (docs/optimize.md) holds up on the 66 local models, against three goals:

1. **A sensible upload** comes out mostly visually intact, nearly as small and fast-loading as a careful manual optimization. KTX2 is the largest win above 512²: textures stay compressed on the GPU, so uploads are much smoother, even though the files are larger.
2. **A careless upload** (low texture occupancy, vertex soup, textures too large, too many faces) is handled mostly right: viewable on commodity hardware, phones where possible, and compact, at the cost of some damage when that makes the model significantly more usable.
3. **A mostly-ok output in a few seconds** for normal models, degrading gracefully as inputs get worse, down to a fully unoptimized output rather than a run that hangs.

The first plan ran first; its gaps became corrections, and the corpus ran again on the plan with them ([With the corrections](#with-the-corrections)), which docs/optimize.md describes and [optimize-plan/current.svg](../optimize-plan/current.svg) draws.

## Method

`tools/experiments/optimize/survey.sh` runs `inspect`, then `optimize --json`, on every local model, smallest first, with a 10-minute cap; `summary.py` reads the reports into one row per model: the first plan's ([slow](optimize-survey-before-slow.csv), [fast](optimize-survey-before-fast.csv)), and the corrected plan's ([slow](optimize-survey-slow.csv), [fast](optimize-survey-fast.csv)). Release build, 4 cores, `--quality High`, KTX2, then `--preset fast`. GPU memory is estimated from image sizes and codecs, with mipmaps: PNG, JPEG, WebP and AVIF upload as RGBA8 (4 bytes a texel), ETC1S as BC1 or ETC2 (0.5), UASTC as BC7 or ASTC (1).

## Results of the first plan

Every model went through, slow and fast: no failure, no timeout.

**The common case is already right.** 19 models are a 200,000-face scan with one 8k texture, a careful export from a photogrammetry tool. Slow takes 19.5 s (median); 17 MB becomes 4.3 MB, the GPU's 341 MB of RGBA8 becomes 11 MB of ETC1S at 4096 (High's cap), and 91 % of the faces stay. That is what goal 1 asks for, apart from the time.

**Time goes to texture encoding.**

| | slow (KTX2) | fast (AVIF effort 2, no `uv`) |
| --- | --- | --- |
| Median, 90th percentile, maximum | 20 s, 71 s, 296 s | 3.2 s, 9.1 s, 140 s |
| Over 10 s, over 60 s | 51, 9 models | 6, 1 models |
| Share of the time | `ktx` 72 %, `uv` 19 %, `simplify` 5 % | `avif` 63 %, `simplify` 20 %, `import` 10 % |

Step costs, fitted on the runs (least squares, no intercept), predict a step from the import report:

| Step | Cost |
| --- | --- |
| `ktx` | 0.56 s per output megatexel in ETC1S, 0.74 in UASTC: about 10 s for a 4096² image |
| `avif` effort 2 | 0.13 s per output megatexel |
| `simplify` | 1.4 s per million faces |
| `uv` | 2.4 s per million faces, plus 0.1 s per megatexel |

The fast preset meets goal 3 for normal models: 60 of 66 in under 10 s, a 6× median speedup. The six slower ones are the careless inputs: many large textures (`02-73`: 16 images of 4096², 21 s of AVIF; `Lion`: 14 images; the campus: 634), and the 44-million-face scan (15 s of import, 62 s of `simplify`, 55 s of AVIF).

**`uv` runs where it can't gain.** It repacked 13 of the 27 models below 0.6 occupancy, saving 3 to 57 % of their texels, and none of the 36 above, except three cards of 4 to 12 faces. Above 0.6 it cost up to 18 s a model for nothing: 119 s, 6 % of slow's total time. Where it repacks an 8k atlas, `ktx --max-size 4096` scales the result down anyway: the gain is texel density at 4096, a sharper texture, not a smaller one.

**Careless inputs aren't brought to budget.** There is no face or texture budget, only `simplify`'s error and a per-image side cap:

| Model | Faces out | GPU textures out | Download | Slow time |
| --- | --- | --- | --- | --- |
| `MAN63675_HP_44M` (44 M faces, 16 × 4096²) | 2.24 M | 299 MB | 108 MB | 296 s |
| `Lion` (14 images up to 4096²) | 181,000 | 213 MB | 55 MB | 107 s |
| `Lion` (WebP textures) | 247,000 | 171 MB: WebP isn't decoded, so stays RGBA8 | 8 MB | 1 s |
| `02-73` (16 × 4096²) | 423,000 | 166 MB | 32 MB | 137 s |

Every other model ends under 1 M faces and 64 MB of GPU textures. Nine keep more than 500,000 faces.

**Some outputs grow without a gain.**
- Already compressed inputs: `Ebauche_statue_grecque` (Draco and KTX2) grows from 1.8 to 3.4 MB, and the two compressed `Lion` files by 10 to 33 %. `simplify` rewrites their geometry, and meshopt at 14 bits is larger than their Draco. Their textures were KTX2 already, or WebP, which `ktx` leaves alone.
- KTX2 itself: UASTC normal and occlusion maps are 2 to 3 times their JPEG (`tasse` 5.5 → 14 MB, `athlete` 21 → 30 MB). That is the price goal 1 accepts for GPU memory.

## What the first plan got wrong

| Goal | Today | Correction |
| --- | --- | --- |
| 1 | `uv` runs up to 0.9 occupancy, for nothing above 0.6 | `uv` below 0.6 occupancy only: 6 % of slow's time on this corpus, up to 18 s a model |
| 1 | Images of 512² and less go to KTX2 | Keep them as they are: GPU memory is negligible there |
| 1 | Already compressed inputs grow | Keep compressed geometry under the face budget as it is; a final guard keeps the input when the output is larger and saves no GPU memory |
| 2 | No face budget: the 44M scan keeps 2.24 M faces at High | `simplify --max-faces` (new): 1 M at High, as eCorpus's simplify ratio targets 1 M vertices |
| 2 | No texture budget: 166 to 299 MB of GPU textures | One 64 MB GPU budget for geometry and textures: lower one maximum side for the whole model until it fits, or cut faces first when the texture density is low ([Budgets](#budgets)) |
| 2 | WebP and AVIF sources stay RGBA8 on the GPU | Decode them for `ktx` (already a TODO for AVIF) |
| 3 | fast's time grows with texels and faces, unbounded | Estimate each step from the cost table; in fast, shrink images until AVIF fits about 6 s, skip `simplify` past about 3 s (unsimplified geometry, with a warning) |
| 3 | A failing step fails optimize | Keep the stage before it, warn, carry on; at worst the import stage is the output |

The corrections went in as proposed, with these changes. There is no `simplify --max-faces`, and the budget went in as 256 MB for geometry and textures, with no face ceiling: models too heavy at the quality's error stay over budget, with a warning, since the plan is best effort and shouldn't damage them further. In fast, `simplify` always runs: a model left with all its faces can be too heavy to look at, which is worse than a long wait. And small images are encoded after all when there are enough of them to keep a model over budget ([below](#with-the-corrections)).

## Budgets

Geometry is cheap next to textures. After `compress`, 1 M faces cost about 8 MB to download and 20 MB of GPU memory (16 bytes a quantized vertex, 12 bytes a face with 32-bit indices); one 4096² ETC1S texture costs 2.5 MB and 11 MB, a UASTC one twice that. So one GPU budget for geometry and textures together, 64 MB, with a 1 M face ceiling, is phone-sized without simplifying ordinary scans: `tools/experiments/optimize/budget.py` simulates it on the slow survey.

| Rule (64 MB for geometry and textures) | Models changed | Over budget after |
| --- | --- | --- |
| 1 M faces, then lower one maximum side for the whole model, 256 at a time, until it fits | 5 | 0 |
| 500,000 faces, then the same | 12 | 0 |
| 1 M faces, and at most 1 face per 16 used color texels | 12 | 0 |
| 1 M faces; over budget, cut faces to 16 used texels a face before halving images | 5 | 0 |

The five over budget are the careless models plus two large ones: `02-73` (textures 166 → 54 MB, at 2304), `Lion` (213 → 55 MB, at 2048), `MAN63675_HP_44M` (1 M faces, 21 MB; textures 299 → 42 MB, at 1536), `johannisfriedhof` (64 → 49 MB, at 3584) and `trema-retablebelvaux` (18 MB of geometry; textures 53 → 41 MB, at 3584).

Textures shrink through one maximum side for the whole model, as `ktx --max-size` applies it, not image by image: halving a material's normal map while its color map stays at full size makes no sense. Halving every image at the largest side together overshoots, since each step divides their memory by 4 (`trema` would end at 13 MB of textures for 46 available); steps of 256 fill the budget.

**Texel density** (used color texels a face: texture texels × occupancy / faces) tells geometry that is denser than its texture can show. The careful 200,000-face exports have about 50 at 4096; the 44M scan has 1 at input. As a ceiling of its own, it only cuts ordinary 600,000 to 1 M face scans (Pietà, Mascaron, Chapiteau) that are well under budget: about 5 MB of GPU memory saved each, for damage. Its use is as the tie-breaker once over budget: below the target density, cut faces first; above it, lower the textures' side first. On this corpus that changes one model, `trema` (854,000 → 550,000 faces, its textures at 3840 rather than 3584). The target, 16, is a guess that renders have to settle. Density is undefined without a color texture (a megatexel at least): `Lion`'s normal-map-only files, models with a vertex-color-like 300² texture.

## With the corrections

The same 66 models, slow and fast, on the corrected plan:

| | slow (KTX2) | fast (AVIF effort 2, no `uv`) |
| --- | --- | --- |
| Median, 90th percentile, maximum | 18 s, 75 s, 233 s | 3.3 s, 7.5 s, 94 s |
| Over 10 s, over 60 s | 50, 10 models | 3, 1 models |
| Share of the time | `ktx` 78 %, `uv` 14 %, `simplify` 5 % | `avif` 54 %, `simplify` 24 %, `import` 12 % |
| Estimated GPU memory out, at most | 245 MB; 5 models over 64 MB | 373 MB; 1 model over 256 MB |
| Input kept (`input-kept`) | 3 | 5 |

- **The common case doesn't change:** the 18 200,000-face scans with one 8k texture take 18 s (median), 17 → 4.5 MB, 16 MB of GPU memory.
- **`uv`** runs on the 27 models below 0.6 occupancy only: 14 % of slow's time, from 19 %.
- **The careless models fit 256 MB in slow:** `02-73` 174 MB and `Lion` 217 MB, both at 4096, `MAN63675_HP_44M` 245 MB at 3328, from 299 MB of textures alone. The 256 MB budget lowers the side of that scan only.
- **The campus scan's 634 images** are mostly 512 or less. Left as RGBA8, as correction 2 had it, they came to 311 MB; the plan now encodes small images too when they keep a model over budget, and it ends at 59 MB (77 s, 60 s of it `ktx`).
- **Compressed inputs** come out as they went in (`input-kept`): `Ebauche` with KTX2, `Lion` with KTX2, and the box of 8 faces. With WebP textures they grow but save GPU memory, so they're kept as optimized: `Ebauche` 2.2 → 3.4 MB for 28 → 9 MB, `Lion` 6 → 24 MB for 178 → 39 MB.
- **fast:** 63 of 66 under 10 s. `trema` (11 s) and `Lion` (10 s) have many large images, and their sides go down to 3840 and 1792 for time. The 44M scan takes 94 s, 59 of them `simplify`, and gives a 16 MB model of 2.24 M faces and 240 MB. The campus, at 373 MB in AVIF, is the one model over budget, with a warning: its images are at 1024 or less already, and AVIF uploads as RGBA8.

## Open questions

- **A careful manual reference.** Nothing here measures goal 1's "as small as a careful manual optimization" directly: the 200k exports are careful on geometry, and `ktx`'s settings match eCorpus's within a few percent ([ktx.md](../ktx.md)). gltfpack (`-cc -tc`) would be an independent reference; it isn't installed.
- **Damage.** The renders (docs/validation.md, field notes) cover eight models, all sensible. The budget damages careless models on purpose: render `MAN63675_HP_44M` at 3328 and the fast outputs at reduced sides against their inputs.
