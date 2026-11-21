# Vendored dependencies

Third-party code compiled into the binaries. It builds as the `meshtinker-vendor`, `meshoptimizer`, `draco-decoder`, `basisu` and `xatlas` targets, without the project's warning flags; its headers are system includes, so their warnings don't reach project code. `impl/` holds the translation units that instantiate the single-header libraries.

| Dependency | Version | License | Use |
| --- | --- | --- | --- |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT, notice in `cgltf.h` | glTF and GLB parsing |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.7.2 | BSD 3-Clause, `CLI11-LICENSE` | Command-line parsing and generated help |
| [Draco](https://github.com/google/draco/tree/1.5.9) | 1.5.9, decoder only (`draco/src/`) | Apache 2.0, `draco/LICENSE` | `KHR_draco_mesh_compression` decoding |
| [meshoptimizer](https://github.com/zeux/meshoptimizer/tree/v1.2) | 1.2 (`src/` only) | MIT, `meshoptimizer/LICENSE.md` | `EXT_meshopt_compression` and `KHR_meshopt_compression` decoding, `simplify`, and `compress`'s reordering, filters and encoding |
| [JSON for Modern C++](https://github.com/nlohmann/json/tree/v3.11.3) | 3.11.3 | MIT, notice in `nlohmann/json.hpp` | Passing the glTF JSON through, report output |
| [stb](https://github.com/nothings/stb) | stb_image 2.30, stb_image_write 1.16 | MIT or public domain, notice in each header | PNG and JPEG decoding and PNG writing, for texture work |
| [stb_image_resize2](https://github.com/nothings/stb_image_resize/tree/a4f303e34a92d1410d510cbe85a1781ce4b8c9fd) | 2.51 | MIT or public domain, notice in the header | Resizing images (`ktx` and `uv` `--max-size`) |
| [basis_universal](https://github.com/BinomialLLC/basis_universal/tree/v2_50) | 2.50 (`encoder/`, `transcoder/`, `zstd/`) | Apache 2.0, `basisu/LICENSE` and `basisu/NOTICE`; bundled code under `basisu/LICENSES/` | KTX2 encoding for `ktx`, and decoding in its tests |
| [xatlas](https://github.com/jpcy/xatlas/tree/f700c7790aaa030e794b52ba7791a05c085faf0c) | f700c77 (2022-07-26, `source/xatlas/`) | MIT, `xatlas/LICENSE` | Chart packing for `uv` |

All copies are unmodified. Draco comes from upstream (below); the others come from `../cluster_lod/packages/tools/vendor`, which took them from upstream:

- `cgltf.h` is the single header bundled with meshoptimizer 1.2. cgltf's writer is not vendored: meshtinker writes glTF JSON itself so that unknown extensions pass through untouched.
- `CLI/` is upstream's `include/CLI/`.
- `meshoptimizer/src/` is the library source of v1.2. SHA-256 sums of the vendored files are in `meshoptimizer/SHA256SUMS`.
- `draco/src/draco/` holds the sources of Draco 1.5.9's default decoder build (the source lists of its `draco_decoder` target in `CMakeLists.txt`, minus the JavaScript glue), plus every header those sources include that the lists omit. This machine has Debian's `libdraco-dev` in `/usr/include/draco`, which hides a missing header locally: after updating Draco, check that no vendored file includes a system Draco header (`g++ -MM ... | grep /usr/include/draco`), as CI has no system copy. It comes from the release archive (`https://github.com/google/draco/archive/refs/tags/1.5.9.tar.gz`, SHA-256 `5b84e0e212ccc7b03ace681760a65a66fe2481a5f54df65154cfc8f0cbb87148`), the version eCorpus's `draco3dgltf` is built from. `draco_features.h`, which Draco's CMake generates, is written by hand with the default build's features. SHA-256 sums of the vendored files are in `draco/SHA256SUMS`. It builds as the `draco-decoder` target.
- `nlohmann/json.hpp` is upstream's `single_include/nlohmann/json.hpp` at v3.11.3, with its SHA-256 in `nlohmann/SHA256SUMS`.
- `stb/` holds `stb_image.h` and `stb_image_write.h`, and `stb_image_resize2.h` from its own repository (it moved out of `nothings/stb` in November 2026), at the commit linked above.
- `basisu/` is the v2_50 tag (commit `9bebe16726b3a61c8c213eeee3b7cffb462ef34e`): `encoder/` without the WebAssembly API files (`basisu_wasm_*`), `transcoder/`, and from `zstd/` the single-file `zstd.c` with its headers and license. The `basisu` target compiles upstream's `ENCODER_LIB_SRC_LIST` with Zstandard on, OpenCL and astcenc off, and SSE 4.1 on x86-64 as upstream does: one of its kernels is called without a CPU check, so `ktx` needs an x86-64-v2 processor. SHA-256 sums are in `basisu/SHA256SUMS`.
- `xatlas/` holds `xatlas.cpp` and `xatlas.h` from upstream's `source/xatlas/`, with SHA-256 sums in `xatlas/SHA256SUMS`.

Check the sums with `sha256sum -c SHA256SUMS` in each folder.
