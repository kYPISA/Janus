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
  Integrated into the container as `prefilter_id = 3`. Grammar and format details:
  [JANUS_NOTATION_SPEC.md](JANUS_NOTATION_SPEC.md).
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

None. Brotli 1.1.0 and the LZMA SDK are included in this repo, so the only thing you need is a compiler.

## Building

You don't need to install any libraries. Brotli 1.1.0 and the LZMA SDK are included in this repo, so the only thing you need is a compiler with C++17 support.

Pre-built Linux and Windows binaries are produced automatically by GitHub Actions on every push (see `.github/workflows/build.yml`) — grab them from the Actions artifacts or the releases page.

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
| Windows | Fully working, tested               | Built by GitHub Actions         |
| Linux   | Compiles and runs (build from source) | Pre-built via GitHub Actions   |
| macOS   | Should compile, not tested yet      | Coming in a future update      |

Official pre-built binaries for Linux and macOS will be provided in upcoming releases.

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

Segments can go through [Janus Notation](JANUS_NOTATION_SPEC.md) before Brotli/LZMA:
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
