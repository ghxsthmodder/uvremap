# Third-party dependencies

All dependencies are vendored under `third_party/`, pinned to the commits below.

| Library | Upstream | Commit | Date | License |
|---|---|---|---|---|
| xatlas | https://github.com/jpcy/xatlas | `f700c77` | 2022-07-26 | MIT |
| tinygltf | https://github.com/syoyo/tinygltf | `488a70a` | 2025-11-02 | MIT |
| stb | https://github.com/nothings/stb | `003501f` | 2026-10-22 | Public Domain / MIT ("enhancements") |

License texts: `third_party/<lib>/LICENSE`.

Usage notes:

- **xatlas** is built as a static library (`third_party/xatlas/CMakeLists.txt`) and
  provides chart packing/parametrization (`src/core/xatlas_runner.cpp`).
- **tinygltf** is header-only and compiled with `TINYGLTF_IMPLEMENTATION` in a single
  translation unit (`src/core/gltf_io.cpp`), with `TINYGLTF_NO_STB_IMAGE` and
  `TINYGLTF_NO_STB_IMAGE_WRITE` — image decoding/encoding goes through our own stb path.
- **stb** provides PNG/JPG/HDR IO (`image.h`/`image.cpp`) and `stb_image_write` for
  output images.

## Samples

`samples/` is a clone of https://github.com/KhronosGroup/glTF-Sample-Assets, used only
for verification (`scripts/verify/`). It is not part of the shipped tool.
