# Images

How commands that decode images (`uv`, `ktx`) read, hold, resize and write them. The shared code is the `image` namespace (`include/meshtinker/image/`, in `meshtinker-core`), and [uv.md](uv.md) and [ktx.md](ktx.md) build on it.

## Formats

- **PNG and JPEG are decoded** with stb_image (already vendored), from the mapped file (`stbi_load_from_memory`), so the compressed file is never copied.
- **Other formats are copied unchanged** with one counted warning, as [normalized-profile.md](normalized-profile.md) says for every command that decodes images. WebP and AVIF are `image-format-unsupported`.
- **KTX2 is the end of the line.** `ktx` runs after every other image command. A command that meets a KTX2 image leaves it as it is and warns with `image-already-compressed`, whose message says to run that command before `ktx`. A command that rewrites geometry handles data that was already compressed the same way: it doesn't re-encode it ([normalized-profile.md](normalized-profile.md#commands-that-rewrite-geometry)). Decoding KTX2 to bake it again would add a second lossy round for no gain.
- **8 bits per channel.** 16-bit PNG is reduced to 8 bits on decoding (`stbi_load_from_memory` does it). basisu's LDR modes take 8 bits, glTF renderers sample 8-bit textures, and nothing downstream can export more precision, so keeping it would only cost space.
- **Pixels are passed as stored.** There is no color management: PNG `gAMA`/`iCCP` chunks and JPEG ICC profiles are ignored, as glTF renderers ignore them. The material slot alone decides whether the data is sRGB color or linear data (see [ktx.md](ktx.md#codec-per-slot)).

## Channel analysis

Before encoding, a decoded image is checked for:

- **unused alpha:** every alpha value is 255, or every material that samples the image ignores alpha (`alphaMode: OPAQUE`, or a slot that doesn't read alpha). The image is then encoded as RGB.
- **gray:** R = G = B on every texel. An encoder that stores one channel could use it; basisu writes RGB or RGBA only, so `ktx` doesn't.

## Resizing

Resizing is an option of the commands that already decode images (`ktx --max-size`, `uv --max-size`), not a command of its own. A separate `resize` would add a decode and encode round trip of every image to the chain, which is slow on 16k textures. A standalone `resize` can come later if a pipeline needs smaller images without KTX2.

- `--max-size N` caps the longest side at N texels and keeps the aspect ratio. Images are never enlarged by it.
- KTX2 output also rounds each side up to a multiple of 4, which `KHR_texture_basisu` requires ("`pixelWidth` and `pixelHeight` MUST be multiples of 4"). Rounding up keeps every texel. UVs are normalized, so a slight stretch doesn't move anything on the model.
- The resampler is stb_image_resize2 (to vendor, same author and license as stb_image), with its default filters. sRGB slots are resampled in linear light and alpha is premultiplied while filtering, using stb_image_resize2's own options. Normal maps are renormalized afterwards.

## Memory and threads

Images break the "map, don't load" rule: the decoders and encoders need whole images in memory, and an 8k RGBA image is 256 MiB decoded, a 16k one 1 GiB. Memory stays low where it is easy, without working around the libraries to get it there:

- Source files are mapped, never copied. Outputs are written as they are produced.
- **One image at a time**, as long as that keeps the cores busy: work inside an image (basisu's job pool, baking rows) is split across threads. Small images, which can't keep every core busy on their own, are processed several at a time: `ktx` encodes images under a megatexel (1024²) in batches of one per thread, each on one thread, and writes a batch's files once it is done.
- Decoded pixels are on the heap. Peak memory is about the largest image times a factor that depends on the command (measured in [ktx.md](ktx.md) and [uv.md](uv.md)).
- The thread count is the number of CPUs the process may use: its affinity mask (`sched_getaffinity`), capped by the cgroup's `cpu.max` quota when one is set.

This sits under the "flexible memory management" item in TODO.md: commands decide their own memory use, and meta-commands pick commands according to it.

## Writing

- KTX2 is written by basisu ([ktx.md](ktx.md)).
- `uv`'s baked images are written as PNG with stb_image_write (already vendored), at a fast compression level (`stbi_write_png_compression_level` 1). PNG adds no lossy round, and `ktx` usually follows, so size matters less than writing time.
- A file name keeps its stem and changes its extension (`wall.png` becomes `wall.ktx2`). Collisions are renamed as in [gltf-io.md](gltf-io.md#writing).
- Encoding to JPEG or KTX2 is a lossy round. In a chain it goes into `lossy`, and a second round on the same image warns with `repeated-lossy-image-encoding` ([chaining.md](chaining.md)). In file mode there is no history, and a JPEG source isn't counted.
