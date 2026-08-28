# Janus Engine

Hi. This is the very first version of something I've been messing around with. It's called Janus — right now it's a compression tool, but the actual idea is bigger than that. I want it to become a kind of language/format that AI models can read and write more naturally, and I'm trying to build the whole pipeline (including a server) from scratch with whatever I can cobble together.

So yeah, this is experimental. Very experimental. Don't use it for anything important yet.

## What it actually does right now

It compresses and decompresses files using a `.janus` container format. It picks between:

- **Brotli level 11** — good for text-like stuff
- **LZMA ultra** — good for structured binary data
- **Raw storage** — fallback if compression makes the file bigger

It also has a fixed RLE prefilter and splits large files into 128 MB segments.

## Libraries you need to install

None. Brotli 1.1.0 and the LZMA SDK are included in this repo, so the only thing you need is a compiler.

## Building

You don't need to install any libraries. Brotli 1.1.0 and the LZMA SDK are included in this repo, so the only thing you need is a compiler with C++17 support.

A Windows binary (`janus.exe`) is included in this repo. Pre-built Linux and Windows binaries are also produced automatically by GitHub Actions (see `.github/workflows/build.yml`).

### Windows (MinGW)

I built this with TDM-GCC 10.3.0. If you have MinGW:

```bash
mingw32-make -j4
```

Clean:

```bash
mingw32-make clean
```

### Linux / macOS

```bash
make -j4
```

Clean:

```bash
make clean
```

If your `g++` doesn't have `libstdc++` static libraries installed, the `-static-libstdc++` flag in the Makefile might fail. In that case just remove that flag (and `-static-libgcc` if needed) from the `LDFLAGS` line.

### CMake

There's also a `CMakeLists.txt` if you prefer:

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j4
```

You'll need CMake 3.10+ and a compiler with C++17 support.

## Platform support

| OS      | Status                              | Binary                         |
|---------|-------------------------------------|--------------------------------|
| Windows | Fully working, tested               | `janus.exe` included           |
| Linux   | Compiles and runs (build from source) | Pre-built via GitHub Actions   |
| macOS   | Should compile, not tested yet      | Coming in a future update      |

Official pre-built binaries for Linux and macOS will be provided in upcoming releases.

## Usage

```bash
# Compress
janus -c input.txt output.janus
janus -c input.txt                    # creates input.txt.janus

# Decompress
janus -d archive.janus output.txt
janus -d archive.janus                # creates archive.unjanus
```

## What's in the repo

- `main.cpp` — CLI and the compress/decompress loop
- `janus_engine.hpp` — format headers, heuristics, segmentation, filters
- `stream_io.hpp` — buffered streaming I/O
- `C/` — LZMA SDK (public domain, see `LICENSE-LZMA-SDK.txt`)
- `brotli-1.1.0/` — Brotli source (MIT)
- `Makefile` / `CMakeLists.txt` — pick your poison

## Important disclaimer

This is basically version 0.0.1. It's better suited for editing, learning, and breaking than for real use. The format will probably change. A lot. Don't archive your only copy of anything with it.

## Why I'm doing this

I set myself a challenge: build a server from scratch, using whatever junk I had lying around, and try to train my own AI model on a custom data/language format. Janus is the first step of that. If you're looking at this and thinking "this is weird," good. It is.

## License stuff

- The original Janus code (`main.cpp`, `janus_engine.hpp`, `stream_io.hpp`) doesn't have a license yet. It's just a prototype.
- LZMA SDK is public domain — see `LICENSE-LZMA-SDK.txt`.
- Brotli is MIT licensed — see `brotli-1.1.0/LICENSE`.

## Support & feedback

If you want to support the madness:  
https://boosty.to/kypisa.verif

If you tried it and have thoughts:  
https://forms.gle/8nZY7rUxWygbKjt27

If you're editing this project — thank you so much. It means the world to me. <3
