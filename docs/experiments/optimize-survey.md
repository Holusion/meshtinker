# Experiment: optimize's plan against the corpus

How `optimize`'s plan (docs/optimize.md) holds up on the 66 local models, against three goals:

1. **A sensible upload** comes out mostly visually intact, nearly as small and fast-loading as a careful manual optimization. KTX2 is the largest win above 512²: textures stay compressed on the GPU, so uploads are much smoother, even though the files are larger.
2. **A careless upload** (low texture occupancy, vertex soup, textures too large, too many faces) is handled mostly right: viewable on commodity hardware, phones where possible, and compact, at the cost of some damage when that makes the model significantly more usable.
3. **A mostly-ok output in a few seconds** for normal models, degrading gracefully as inputs get worse, down to a fully unoptimized output rather than a run that hangs.

Diagrams: [the plan today](../optimize-plan/current.svg) and [the plan proposed below](../optimize-plan/proposed.svg) (Graphviz sources beside them).

## Method

`tools/experiments/optimize/survey.sh` runs `inspect`, then `optimize --json`, on every local model, smallest first, with a 10-minute cap; `summary.py` reads the reports into one row per model ([slow](optimize-survey-slow.csv), [fast](optimize-survey-fast.csv)). Release build, 4 cores, `--quality High`, KTX2, then `--preset fast`. GPU memory is estimated from image sizes and codecs, with mipmaps: PNG, JPEG, WebP and AVIF upload as RGBA8 (4 bytes a texel), ETC1S as BC1 or ETC2 (0.5), UASTC as BC7 or ASTC (1).

## Results

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

## What the current plan gets wrong

| Goal | Today | Correction |
| --- | --- | --- |
| 1 | `uv` runs up to 0.9 occupancy, for nothing above 0.6 | `uv` below 0.6 occupancy only: 6 % of slow's time on this corpus, up to 18 s a model |
| 1 | Images of 512² and less go to KTX2 | Keep them as they are: GPU memory is negligible there |
| 1 | Already compressed inputs grow | Keep compressed geometry under the face budget as it is; a final guard keeps the input when the output is larger and saves no GPU memory |
| 2 | No face budget: the 44M scan keeps 2.24 M faces at High | `simplify --max-faces` (new): 1 M at High, as eCorpus's simplify ratio targets 1 M vertices |
| 2 | No texture budget: 166 to 299 MB of GPU textures | Lower the largest images' side until the estimate fits 128 MB at High |
| 2 | WebP and AVIF sources stay RGBA8 on the GPU | Decode them for `ktx` (already a TODO for AVIF) |
| 3 | fast's time grows with texels and faces, unbounded | Estimate each step from the cost table; in fast, shrink images until AVIF fits about 6 s, skip `simplify` past about 3 s (unsimplified geometry, with a warning) |
| 3 | A failing step fails optimize | Keep the stage before it, warn, carry on; at worst the import stage is the output |

With these, the estimates for the four careless models at High: the 44M scan simplifies to 1 M faces (about 60 s) and its textures fit 128 MB at 2048 (about 45 s of `ktx`); `02-73` and `Lion` fit 128 MB at 2048 for some images, in about 40 s instead of 107 to 137 s. Fast stays near 10 s for all but the 44M scan, whose import and weld alone take 22 s: its fast output is unsimplified geometry with small AVIF textures, the "fully unoptimized" end of goal 3.

## Open questions

- **Phone budgets.** 1 M faces and 128 MB are desktop-safe guesses that only bite on the four careless models. A phone budget (500,000 faces, 64 MB) would also simplify the nine models between 500,000 and 1 M faces. Which device is the target, at which quality?
- **A careful manual reference.** Nothing here measures goal 1's "as small as a careful manual optimization" directly: the 200k exports are careful on geometry, and `ktx`'s settings match eCorpus's within a few percent ([ktx.md](../ktx.md)). gltfpack (`-cc -tc`) would be an independent reference; it isn't installed.
- **Damage.** The renders (docs/validation.md, field notes) cover eight models, all sensible. The budget corrections damage careless models on purpose; render them once implemented.
