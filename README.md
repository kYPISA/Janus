# Janus Engine

Hi. This is the very first version of something I've been messing around with. It's called Janus — right now it's a compression tool, but the actual idea is bigger than that. I want it to become a kind of language/format that AI models can read and write more naturally, and I'm trying to build the whole pipeline (including a server) from scratch with whatever I can cobble together.

So yeah, this is experimental. Very experimental. Don't use it for anything important yet.

## What it actually does right now

Janus is a **unified hybrid compression engine**: one system, not two separate tools. It
compresses and decompresses files into `.jan` archives. Every file is split into segments,
and for each segment the engine runs real compression trials and keeps whichever path
actually produces the smallest payload:

- **Brotli level 11** — good for text-like stuff
- **LZMA ultra** — good for structured binary data
- **Raw storage** — fallback if compression makes the file bigger

Before the backend runs, an integrated preprocessing stage can rewrite the segment so the
compressor is fed cleaner, more uniform data: RLE, Delta, or **Janus Notation**, a token
substitution transform for structured data. Janus Notation is not a separate tool or a
separate format you invoke — it is `prefilter_id = 3` inside the same container, applied
automatically when a segment contains genuine structural repetition. It also ships as a
standalone embeddable library (`libjanus_notation`) if you want the transform on its own.

Every prefilter and engine decision is confirmed by an actual compression trial, not a
heuristic guess: if a transform doesn't beat the plain path, the segment is stored
unfiltered. That is why the combined system beats plain Brotli/LZMA on structured input
while never losing to them on anything else.

Large files are split into 128 MB segments.

## Current status

Implemented and verified as of 2026-08-29:

- **v3 segmented container format** — 22 bytes of overhead for a single-segment archive
  (8-byte header + 14-byte segment table entry). Older v2 archives (46-byte overhead)
  remain fully readable; the decompressor accepts both layouts.
- **Adaptive engine selection by real compression trial** — each segment is actually
  compressed with both Brotli 11 and LZMA Ultra, and the smaller payload wins; raw storage
  is the fallback. No heuristic guessing.
- **Adaptive prefilters** — RLE, Delta, and Janus Notation, each applied only when a
  compression trial confirms a real improvement; rejected segments are stored unfiltered.
- **Janus Notation** — standalone embeddable library (`libjanus_notation`) with a formal
  grammar: a fixed core vocabulary plus an adaptive vocabulary built per segment.
  Integrated into the container as `prefilter_id = 3`. The implementation is in
  [`lib/`](lib/).
- **Default file extension changed from `.janus` to `.jan`** (decompressed default:
  `.unjan`). Archives are detected by magic bytes, not file extension, so existing
  `.janus` archives still open and decompress with no flags.
- **Debug / benchmark flags** — `--raw-brotli` and `--raw-lzma` (single-codec baselines
  that skip the container) and `--log <path.json>` (JSON benchmark log: sizes, ratio,
  timing, peak memory, per-segment detail).

Build state: clean rebuild with zero warnings; notation test suite 49/49 passing.

### Container layout

Archives are a small fixed header followed by a segment table and the segment payloads,
stored contiguously in table order (payload offsets are implicit):

```
header (8 bytes):    magic "ANUS" (0x53554E41), version (0x03), container_mode, segment_count
segment table:       one 14-byte entry per segment:
                     algo_id, prefilter_id, compressed_size (u32),
                     original_size (u32), segment_crc32 (u32)
segment payloads:    back to back, in segment order
```

Fixed overhead is 22 bytes for a single-segment archive and 14 bytes per additional
segment. Older version-2 archives (46-byte single-segment overhead) remain readable —
the decompressor accepts both layouts.

## Libraries you need to install

No separate codec libraries. Brotli 1.2.0 and the required LZMA SDK sources are included in this repo; build prerequisites are listed below.

## Building

The build uses CMake 3.16+, a C++17 compiler, and Python 3 for integration tests. Brotli 1.2.0 and the LZMA SDK are bundled; no separate codec installation is needed.

GitHub Actions builds and tests natively on Windows, Linux, and macOS, then uploads the installed executable, shared library, C header, and licenses. A separate Linux job runs AddressSanitizer and UndefinedBehaviorSanitizer. See `.github/workflows/build.yml`.

Sanitizer CI enables leak detection for the native C/C++ tests. The Python/ctypes integration run preloads ASan and disables leak detection for the uninstrumented Python interpreter; address and undefined-behavior checks remain enabled.

### Native builds (Windows / Linux / macOS)

```bash
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --config Release --parallel 4
ctest --test-dir build/native -C Release --output-on-failure
cmake --install build/native --config Release --prefix dist
```

Windows can use Visual Studio or MinGW (`-G "MinGW Makefiles"` on the configure command). Executables go in `build/native/bin`, libraries in `build/native/lib`; multi-configuration generators add a `Release` subdirectory. Installation places them in `dist/bin` and `dist/lib` with `dist/include/janus_lib.h`.

`python build_libs.py` runs the same configure/build/test sequence. The Makefile is also a CMake wrapper: `make`, `make test`, or `mingw32-make test`. Clean with `cmake --build build/native --target clean`.

### Shared-library API

The C API exports `janus_compress_file`, `janus_decompress_file`, `janus_last_error`, and `janus_version_string`. Include `janus_lib.h` and link the installed import/shared library. The file operations return **1 on success, 0 on failure**; read `janus_last_error()` on the same thread after a failure. `janus_compress_file` takes a thread count, and an optional log path can be null. Decode/codec failures leave an existing destination unchanged; an optional log-write failure is reported after the output has been committed.

### Zig cross-builds from Windows

The supplied toolchains build x86-64 Linux and macOS artifacts using the Python `ziglang` package (verified with Zig 0.14.1):

```bash
python -m pip install ziglang==0.14.1
python build_libs.py --build-dir build/linux --generator "MinGW Makefiles" --toolchain cmake/zig-linux.cmake --no-test
python build_libs.py --build-dir build/macos --generator "MinGW Makefiles" --toolchain cmake/zig-macos.cmake --no-test
```

These batch-file toolchains are for a Windows host with `mingw32-make` available. Cross-builds verify compilation/linking, not runtime behavior. Run the native test suite on the target OS before treating these artifacts as runtime-verified.

Zig 0.14.1 does not support Darwin export-list linker flags, so the macOS cross-built library also exposes some bundled C++ runtime helpers. Native macOS builds restrict exports to the four C API functions.

## Platform support

| OS      | Local verification on the Windows development host | Native CI |
|---------|----------------------------------------------------|-----------|
| Windows | MinGW executable/shared library; all CTest targets pass | Build, test, install (MSVC) |
| Linux   | x86-64 executable/shared-library cross-build and format/export checks | Build, test, install; sanitizers |
| macOS   | x86-64 executable/shared-library cross-build and format/export checks | Build, test, install |

Linux/macOS runtime results and MSVC results require their native CI runs; cross-compilation alone does not establish them.

## Usage

```bash
janus -c input.txt output.jan
janus -c input.txt
janus -d archive.jan output.txt
janus -d archive.jan
```

Compressing without an output path creates `<input>.jan`; decompressing without one
creates `<input>.unjan`. Archives are identified by their magic signature, not by the
file name — older archives saved with the `.janus` extension open and decompress exactly
the same, no flags needed.

### Benchmark logging (`--log`)

Any compress/decompress run can write a JSON benchmark log with sizes, ratio,
timing, and peak memory usage (on Windows, peak memory comes from
`GetProcessMemoryInfo`; the build links `psapi` for this):

```bash
janus -c test.json janus.out --log janus.json
```

Example log for the container path (per-segment details are included):

```json
{
  "timestamp": "2026-08-29T07:14:46Z",
  "mode": "janus-container",
  "input": "test.json",
  "original_size": 452930,
  "compressed_size": 42148,
  "ratio": 0.0931,
  "compression_time_s": 0.327,
  "peak_memory_bytes": 57204736,
  "segments": [
    {"index": 0, "payload_offset": 22, "original_size": 452930, "compressed_size": 42126, "algorithm": "brotli-l11", "prefilter": "none", "notation_applied": false, "notation_size_before": 452930, "notation_size_after": 452930, "crc32": 3488069413}
  ]
}
```

Each segment entry now also reports whether the Janus Notation prefilter was applied
(`notation_applied`) and the segment size before/after the notation transform
(`notation_size_before` / `notation_size_after`).

### The Janus Notation prefilter (`prefilter_id = 3`)

Segments can go through [Janus Notation](lib/janus_notation.hpp) before Brotli/LZMA:
the segment is re-encoded as a token stream (adaptive vocabulary per segment, literal
escaping for bytes the notation grammar reserves). Vocabulary selection is
frequency-thresholded rather than a flat top-48: only tokens of 4–16 bytes that occur at
least `max_token_frequency / 40` times are candidates (capped at 48 slots), so the vocab
size adapts to the data — a file of uniformly repeated words keeps all 48 slots, while a
record-based file selects only the handful of structural tokens that recur in essentially
every record. This matters because mid-frequency long tokens are usually poor candidates:
Brotli already codes them cheaply via back-references, and substituting them away removes
match material and makes the compressed stream *larger*. The
prefilter is adaptive in the same spirit as RLE/Delta — acceptance is two-stage: first a
cheap entropy pre-gate (the notation stream's order-0 Shannon entropy plus the vocabulary
table must be at least 1% below the original's, which skips hopeless segments without a
second compression pass), then a compress trial — the engine actually compresses both the
raw segment and the notation stream and keeps whichever payload is smaller. This makes the
decision immune to entropy-estimation error: if token substitution does not beat the
compression backend on real output size, the segment is stored unmodified and the decision
is recorded in the segment header.

On disk, a `prefilter_id = 3` segment payload is laid out as:

```
[varint] encoding_metadata_length
[bytes]  encoding_metadata        (adaptive vocab table)
[varint] notation_stream_length
[bytes]  Brotli/LZMA stream       (of the notation token stream)
```

Archives written with `prefilter_id = 0/1/2` remain fully readable (old segments never
contain this framing).

## Benchmarks (measured, not projected)

Measured against `--raw-brotli` (Brotli 11 alone) on the current test corpus:

- **Realistic structured JSON** — on a 496 KB synthetic but realistic sensor-telemetry
  export (2,300 records, repeated keys, categorical values, device/location identifiers)
  Notation wins by **3.7%** (29,379 vs 30,505 bytes): the notation transform shrinks the
  segment by 24% and the surviving token stream compresses better than the raw JSON.
  Expect roughly 3-4% on similar data — Brotli's back-references already capture most
  record-level repetition, so Notation's advantage concentrates in the small set of
  very-high-frequency structural tokens.
- **Highly repetitive structured data** — on a synthetic corpus of 48 ten-letter words
  repeated uniformly, Notation wins by **34%**. That is the upper bound, not a typical
  case.
- **Plain prose / unstructured text** — Notation correctly declines to apply: on
  `test.json` the compress trial rejects it (Brotli's own context modeling captures more
  of the structure than token substitution does), and plain English prose fails the
  entropy pre-gate before the trial even runs. The container carries plain Brotli's
  payload with only the 22-byte container header as overhead.

Because acceptance is decided by real compression output size, these are no-regression
results: the worst case is matching the best single backend, not losing to it.

### Hidden debug / benchmark flags

These skip the Janus container entirely and run a single codec over the whole
file. They exist for baseline comparisons during development — the output files
are **not** `.jan` archives. Raw Brotli (quality 11) output is
`[original size: 8 bytes LE][Brotli stream]`; raw LZMA (level 9) output is
`[LZMA props: 5 bytes][original size: 8 bytes LE][LZMA stream]`.

```bash
janus --raw-brotli input.txt output.br
janus --raw-brotli -d output.br restored.txt
janus --raw-lzma input.txt output.lz
janus --raw-lzma -d output.lz restored.txt
```

Both support `--log <log.json>` on the compress side.

## What's next

The next planned module is **notation format extensions** — taking Janus Notation beyond
basic token substitution toward richer structural encodings. No numbers are claimed for it
yet: anything new will go through the same compression-trial acceptance as the existing
prefilters, so it ships only if it measurably helps.

## What's in the repo

- `main.cpp` — CLI and the compress/decompress loop
- `janus_engine.hpp` — format headers, heuristics, segmentation, filters
- `notation_prefilter.hpp` — lossless Janus Notation prefilter integration
- `lib/` — `libjanus_notation` (standalone notation encoder/decoder library)
- `stream_io.hpp` — buffered streaming I/O
- `C/` — required LZMA SDK sources and headers (public domain, see `LICENSE-LZMA-SDK.txt`)
- `brotli-1.2.0/` — Brotli sources, headers, and license (MIT)
- `Makefile` / `CMakeLists.txt` — pick your poison
- `build_libs.py`, `cmake/`, `tools/` — build wrapper and Windows-hosted Zig cross-toolchains
- `tests/`, `.github/workflows/build.yml` — native tests and CI

## Important disclaimer

This is basically version 0.0.1. It's better suited for editing, learning, and breaking than for real use. The format will probably change. A lot. Don't archive your only copy of anything with it.

## Why I'm doing this

I set myself a challenge: build a server from scratch, using whatever junk I had lying around, and try to train my own AI model on a custom data/language format. Janus is the first step of that. If you're looking at this and thinking "this is weird," good. It is.

## License stuff

- The original Janus code is MIT licensed — see `LICENSE`.
- LZMA SDK is public domain — see `LICENSE-LZMA-SDK.txt`.
- Brotli is MIT licensed — see `brotli-1.2.0/LICENSE`.

## Support & feedback

If you want to support the madness:  
https://boosty.to/kypisa.verif

If you tried it and have thoughts:  
https://forms.gle/8nZY7rUxWygbKjt27

If you're editing this project — thank you so much. It means the world to me. <3
