# Janus Notation: Formal Specification and Design Document

## 1. Executive Summary

**Janus Notation** is a formally specified symbolic language designed to serve two simultaneous roles:

1. **Compression prefilter**: Re-represents structured data (JSON, YAML, config files, source code, numeric/tensor metadata) as compact tokens before Brotli or LZMA compression, reducing redundancy and increasing repetition for better final compression ratios.

2. **Human/system-writable language**: A constructed language with a fixed, documented grammar that humans or systems can write directly when they have access to the specification — analogous to ASN.1, Protocol Buffers text format, or S-expressions, but optimized for compression.

**Key design principles:**
- Fixed core vocabulary (≈200–300 tokens) covering universal structural primitives (nesting, arrays, key-value, repetition, common literals).
- Adaptive per-file vocabulary stored in segment headers for domain-specific tokens (function names, JSON keys, repeated string literals).
- Grammar compact enough to fit in an LLM context window (≈2–4 KB reference table).
- Standalone library (`libjanus_notation`) with clean API, usable independently of the `.jan` archiver.
- Zero data loss guaranteed by design — notation is a bijective encoding (lossless round-trip).

---

## 2. Formal Grammar Specification

### 2.1 Token Structure

Janus Notation encodes data as a stream of **tokens**. Each token is one of:

| Token Type | Encoding | Purpose |
|------------|----------|---------|
| **Core symbol** | 1-byte opcode (0x00–0xBF) | Fixed structural primitives |
| **Adaptive symbol** | 1-byte opcode (0xC0–0xEF) + optional payload | File-specific frequent tokens |
| **Literal string** | 0xF0 + varint length + UTF-8 bytes | Uncompressed string data |
| **Literal integer** | 0xF1 + varint (signed zigzag) | Inline numeric literal |
| **Literal float** | 0xF2 + IEEE 754 single (4 bytes) | Inline 32-bit float |
| **Literal double** | 0xF3 + IEEE 754 double (8 bytes) | Inline 64-bit float |
| **Reference** | 0xF4 + varint index | Backreference to prior token sequence |
| **Repetition** | 0xF5 + varint count + token | Repeat next token N times |

**Varint encoding**: unsigned LEB128 (1–5 bytes for 32-bit values). Signed integers use zigzag encoding (0 → 0, -1 → 1, 1 → 2, -2 → 3, etc.).

### 2.2 Core Vocabulary (Fixed, 192 opcodes: 0x00–0xBF)

#### Structural primitives (0x00–0x1F)
| Opcode | Symbol | Meaning |
|--------|--------|---------|
| 0x00 | `NULL` | Null/nil value |
| 0x01 | `TRUE` | Boolean true |
| 0x02 | `FALSE` | Boolean false |
| 0x03 | `{` | Begin object/map |
| 0x04 | `}` | End object/map |
| 0x05 | `[` | Begin array/list |
| 0x06 | `]` | End array/list |
| 0x07 | `:` | Key-value separator |
| 0x08 | `,` | Item separator |
| 0x09 | `(` | Begin tuple/group |
| 0x0A | `)` | End tuple/group |
| 0x0B | `<` | Begin generic container |
| 0x0C | `>` | End generic container |
| 0x0D | `=` | Assignment operator |
| 0x0E | `;` | Statement terminator |
| 0x0F | `\n` | Explicit newline (for structure-sensitive formats) |

#### Common literals (0x20–0x3F)
| Opcode | Symbol | Meaning |
|--------|--------|---------|
| 0x20 | `0` | Integer zero |
| 0x21 | `1` | Integer one |
| 0x22 | `-1` | Integer negative one |
| 0x23 | `""` | Empty string |
| 0x24 | `[]` | Empty array |
| 0x25 | `{}` | Empty object |
| 0x26–0x2F | `2`–`11` | Small positive integers 2–11 |
| 0x30–0x3F | Reserved | Future common literals |

#### Common JSON/config keys (0x40–0x7F, 64 slots)
Examples (actual mapping defined in reference implementation):
- 0x40: `"id"`
- 0x41: `"name"`
- 0x42: `"type"`
- 0x43: `"value"`
- 0x44: `"data"`
- 0x45: `"status"`
- 0x46: `"error"`
- 0x47: `"result"`
- 0x48: `"timestamp"`
- 0x49: `"version"`
- 0x4A: `"config"`
- 0x4B: `"index"`
- 0x4C: `"length"`
- 0x4D: `"size"`
- 0x4E: `"count"`
- 0x4F: `"total"`
- ... (48 more slots for common keys)

#### Common programming constructs (0x80–0xBF, 64 slots)
Examples:
- 0x80: `"class"`
- 0x81: `"function"`
- 0x82: `"return"`
- 0x83: `"if"`
- 0x84: `"else"`
- 0x85: `"for"`
- 0x86: `"while"`
- 0x87: `"import"`
- 0x88: `"include"`
- 0x89: `"public"`
- 0x8A: `"private"`
- 0x8B: `"static"`
- 0x8C: `"const"`
- 0x8D: `"var"`
- 0x8E: `"let"`
- 0x8F: `"true"`
- 0x90: `"false"`
- 0x91: `"null"`
- 0x92: `"undefined"`
- 0x93: `"this"`
- 0x94: `"self"`
- ... (44 more slots)

### 2.3 Adaptive Vocabulary (0xC0–0xEF, 48 slots)

Encoder analyzes the input file and assigns the 48 most frequent domain-specific tokens (function names, variable names, JSON keys not in core vocab, repeated string literals) to this range. The mapping is stored in the **segment header** as:

```
adaptive_vocab_count: varint (0–48)
for each entry:
    token_id: byte (0xC0–0xEF)
    string_length: varint
    string_data: UTF-8 bytes
```

Decoder reads this table before decoding the segment body.

### 2.4 Composition Rules

**Nesting**: Container tokens (`{`, `[`, `(`, `<`) must be balanced with their corresponding close tokens (`}`, `]`, `)`, `>`). Nesting depth is unbounded.

**Repetition shorthand**: `0xF5 <count> <token>` emits `<token>` exactly `<count>` times. Can nest: `0xF5 10 0xF5 5 0x21` → emit `1` fifty times.

**Backreference**: `0xF4 <index>` replays the token sequence starting at byte offset `<index>` in the notation stream until the next structural boundary (end of container or separator). Used for repeated subtrees.

**Whitespace handling**: Janus Notation is whitespace-agnostic unless the source format is whitespace-sensitive (e.g., Python, YAML). In whitespace-sensitive modes, `0x0F` (newline) and a future 0x10 (indent) / 0x11 (dedent) token would be emitted.

---

## 3. Integration with .jan Container Format

### 3.1 Segment Header Extension

Existing `.jan` segment header:
```
segment_id: 4 bytes
uncompressed_size: 8 bytes
compressed_size: 8 bytes
compression_engine_id: 1 byte  // 1 = Brotli, 2 = LZMA
prefilter_id: 1 byte           // 0 = none, 1 = RLE, 2 = Delta
crc32: 4 bytes
```

**New addition** for Janus Notation:
```
prefilter_id: 1 byte
    // 0 = none
    // 1 = RLE
    // 2 = Delta
    // 3 = Janus Notation  ← NEW
    // 4–255 = reserved

encoding_metadata_length: varint  // if prefilter_id == 3
encoding_metadata: bytes          // adaptive vocab table
```

When `prefilter_id == 3`:
- Segment data is: original → Janus Notation encode → Brotli/LZMA compress.
- Decompression: Brotli/LZMA decompress → Janus Notation decode → reconstructed original.

### 3.2 Encoder Decision Logic

For each segment, the encoder:
1. Analyzes content heuristically (syntax, entropy, token frequency).
2. Decides whether Janus Notation is likely beneficial:
   - **Apply Notation** if: structured text (JSON, XML, YAML, source code, config), high redundancy in keys/identifiers.
   - **Skip Notation** if: already compressed (JPEG, PNG, video), random/encrypted data, binary data with no structure.
3. If applied, builds adaptive vocabulary (top 48 frequent tokens not in core vocab), encodes to notation, then compresses with Brotli (for text-like notation) or LZMA (for dense notation).

---

## 4. Worked Examples

### 4.1 JSON Example

**Original JSON** (68 bytes):
```json
{"id":42,"name":"Alice","active":true,"score":98.5,"tags":["a","b"]}
```

**Janus Notation encoding** (conceptual, with opcode references):

```
0x03              // {
0x40              // "id" (core vocab)
0x07              // :
0xF1 0x54         // 42 (varint)
0x08              // ,
0x41              // "name" (core vocab)
0x07              // :
0xF0 0x05 "Alice" // literal string (1 + 1 + 5 = 7 bytes)
0x08              // ,
0xC0              // "active" (adaptive vocab slot 0)
0x07              // :
0x01              // true
0x08              // ,
0xC1              // "score" (adaptive vocab slot 1)
0x07              // :
0xF2 0x42C50000   // 98.5 (IEEE 754 single, 5 bytes)
0x08              // ,
0xC2              // "tags" (adaptive vocab slot 2)
0x07              // :
0x05              // [
0xF0 0x01 "a"     // 3 bytes
0x08              // ,
0xF0 0x01 "b"     // 3 bytes
0x06              // ]
0x04              // }
```

**Byte count**: 1+1+1+2+1+1+1+7+1+1+1+1+1+1+1+5+1+1+1+1+1+3+1+3+1+1 = **41 bytes** (before compression).

**Adaptive vocab header** (3 entries):
```
0x03              // count = 3
0xC0 0x06 "active"
0xC1 0x05 "score"
0xC2 0x04 "tags"
```
= 1 + (1+1+6) + (1+1+5) + (1+1+4) = **22 bytes**.

**Total notation encoding**: 41 + 22 = **63 bytes** (compared to 68-byte original). Further compression with Brotli will exploit repeated structural tokens (`:`, `,`, `{`, `}`) heavily.

### 4.2 Tensor Metadata Example

**Original (text representation, 95 bytes)**:
```
shape: [256, 256, 3]
dtype: float32
order: C
compression: none
checksum: 0xDEADBEEF
```

**Janus Notation** (conceptual):
```
0xC0              // "shape" (adaptive)
0x07              // :
0x05              // [
0xF1 0x80 0x02    // 256 (varint)
0x08              // ,
0xF1 0x80 0x02    // 256
0x08              // ,
0x22              // 3 (small literal)
0x06              // ]
0x0F              // newline
0xC1              // "dtype" (adaptive)
0x07              // :
0xC2              // "float32" (adaptive)
0x0F              // newline
0xC3              // "order" (adaptive)
0x07              // :
0xC4              // "C" (adaptive)
0x0F              // newline
0xC5              // "compression" (adaptive)
0x07              // :
0xC6              // "none" (adaptive)
0x0F              // newline
0xC7              // "checksum" (adaptive)
0x07              // :
0xF1 0xDF 0xFD 0xBB 0xED 0x0E  // 0xDEADBEEF (varint, 6 bytes)
```

**Adaptive vocab** (8 entries): "shape", "dtype", "float32", "order", "C", "compression", "none", "checksum" = ≈50 bytes header.

**Body**: ≈30 bytes. **Total**: ≈80 bytes (16% smaller than original text, before final compression).

---

## 5. Standalone Library Design: `libjanus_notation`

### 5.1 Public API (C++)

**Header**: `janus_notation.h`

```cpp
namespace janus {

struct NotationConfig {
    bool enable_adaptive_vocab;
    size_t max_adaptive_tokens;
    bool whitespace_sensitive;
};

class NotationEncoder {
public:
    explicit NotationEncoder(const NotationConfig& config);
    
    void reset();
    void feed_data(const uint8_t* data, size_t length);
    void finalize();
    
    const uint8_t* get_encoded_data(size_t& out_length) const;
    const uint8_t* get_vocab_header(size_t& out_length) const;
};

class NotationDecoder {
public:
    NotationDecoder();
    
    void set_vocab_header(const uint8_t* header, size_t length);
    void feed_encoded_data(const uint8_t* data, size_t length);
    void finalize();
    
    const uint8_t* get_decoded_data(size_t& out_length) const;
};

enum class NotationError {
    OK,
    INVALID_TOKEN,
    UNBALANCED_CONTAINER,
    TRUNCATED_STREAM,
    ADAPTIVE_VOCAB_OVERFLOW,
    REFERENCE_OUT_OF_BOUNDS
};

NotationError get_last_error();

}
```

### 5.2 Library Packaging

**Deliverables**:
- `libjanus_notation.a` (static) / `libjanus_notation.so` (dynamic, Linux) / `janus_notation.dll` (Windows)
- `janus_notation.h` (public API header)
- `janus_notation_spec.md` (grammar reference, 2–4 KB, embeddable in LLM prompts)
- `examples/` directory with minimal encode/decode sample programs

**Dependencies**: None (pure C++17, STL only).

**Build targets**:
```makefile
# Standalone library (separate from full Janus archiver)
make libjanus_notation        # static library
make libjanus_notation_shared # dynamic library
make install                  # install to /usr/local/lib + /usr/local/include
```

**Integration example** (third-party project):
```cpp
#include <janus_notation.h>

janus::NotationConfig cfg;
cfg.enable_adaptive_vocab = true;
cfg.max_adaptive_tokens = 48;

janus::NotationEncoder enc(cfg);
enc.feed_data(input_data, input_size);
enc.finalize();

size_t encoded_size;
const uint8_t* encoded = enc.get_encoded_data(encoded_size);

// Now compress `encoded` with your own compressor, or use as-is
```

---

## 6. Test Suite Specifications

### 6.1 Integrity Test Suite (Test Suite A)

**Purpose**: Verify zero data loss across the full encode/decode pipeline.

**Test corpus** (minimum 50 files):
- **Text files**: plain text, UTF-8, mixed newlines (LF, CRLF), Unicode emoji
- **Structured data**: JSON (nested, arrays, escaped strings), YAML, TOML, XML, INI
- **Source code**: C++, Python, JavaScript, Rust (syntax edge cases: strings with escapes, comments, long identifiers)
- **Binary data**: raw byte arrays, tensor files (NumPy `.npy`), Protocol Buffers binary
- **Edge cases**:
  - Empty file (0 bytes)
  - Single-byte file
  - File with single repeated byte (`AAAA...`, 1 MB)
  - Random incompressible data (1 MB from `/dev/urandom`)
  - Extremely nested structure (JSON with 1000-level nesting)
  - Very large file (100 MB) tested via streaming API

**Test procedure** (per file):
1. Load original file → `original_data`.
2. Encode with `NotationEncoder` → `notation_data` + `vocab_header`.
3. Compress `notation_data` with Brotli/LZMA → `compressed_data`.
4. Decompress `compressed_data` → `notation_data_decompressed`.
5. Decode with `NotationDecoder` (using `vocab_header`) → `reconstructed_data`.
6. Byte-by-byte comparison: `original_data == reconstructed_data`.

**Pass criterion**: 100% match across all files. Any single-byte mismatch is a **hard failure**.

**Failure reporting**:
- File name
- Byte offset of first mismatch
- Segment ID (if multi-segment file)
- Hex dump of ±16 bytes around mismatch (original vs. reconstructed)
- Encoder/decoder state at failure point (if available)

**Automated run**:
```bash
make test_integrity
# Output:
# [PASS] empty.txt (0 bytes)
# [PASS] sample.json (1234 bytes)
# [FAIL] large_nested.json: mismatch at byte 98765, segment 3
#        Expected: 0x7D  Actual: 0x5D
# ...
# Summary: 49/50 passed (98%). SUITE FAILED.
```

**CI integration**: Must run on every commit. Any failure blocks merge.

### 6.2 Compression Efficiency Test Suite (Test Suite B)

**Purpose**: Quantify compression improvement from Janus Notation vs. baseline.

**Test configurations**:
1. **Baseline**: Plain LZMA-only (no prefilter, no notation).
2. **Full Janus**: Notation + adaptive engine selection (Brotli or LZMA per segment).

**Metrics per file**:
- **Compression ratio**: `original_size / compressed_size`
- **Encode time**: wall-clock seconds
- **Decode time**: wall-clock seconds
- **Throughput**: MB/s (original size / time)

**Comparison report** (example):
```
File: sample.json (10,240 bytes)
---------------------------------------------------------
                   | Ratio | Enc Time | Dec Time | Ratio Δ
---------------------------------------------------------
Baseline (LZMA)    | 3.21x | 0.084s   | 0.012s   | —
Full Janus         | 4.87x | 0.103s   | 0.015s   | +51.7%
---------------------------------------------------------

File: random.bin (1,048,576 bytes)
---------------------------------------------------------
                   | Ratio | Enc Time | Dec Time | Ratio Δ
---------------------------------------------------------
Baseline (LZMA)    | 1.00x | 2.341s   | 0.234s   | —
Full Janus         | 1.01x | 2.456s   | 0.251s   | +1.0%
---------------------------------------------------------

Corpus Summary (50 files):
---------------------------------------------------------
Category           | Avg Ratio (Baseline) | Avg Ratio (Janus) | Δ
---------------------------------------------------------
Structured text    | 3.45x                | 5.23x             | +51.6%
Source code        | 4.12x                | 5.89x             | +43.0%
Binary/tensor      | 2.01x                | 2.34x             | +16.4%
Incompressible     | 1.00x                | 0.99x             | -1.0%
---------------------------------------------------------
```

**Automated run**:
```bash
make test_efficiency
# Runs both configs, generates CSV + markdown report
```

**Separate from integrity test**: Can run independently (integrity must pass first).

---

## 7. LLM Usability: Grammar Reference for Context Inclusion

**Design goal**: The core grammar must fit in ≈2–4 KB so it can be included in an LLM prompt for few-shot encoding/decoding tasks.

**Compact reference format** (`janus_notation_spec.md` excerpt):

```
JANUS NOTATION — QUICK REFERENCE

CORE OPCODES (0x00–0xBF):
0x00=NULL 0x01=TRUE 0x02=FALSE 0x03={ 0x04=} 0x05=[ 0x06=] 0x07=: 0x08=,
0x20=0 0x21=1 0x22=-1 0x23="" 0x24=[] 0x25={}
0x40="id" 0x41="name" 0x42="type" 0x43="value" 0x44="data" 0x45="status"
(full table: 192 fixed tokens, see reference implementation)

LITERAL TYPES:
0xF0 <varint len> <UTF-8 bytes>  → string literal
0xF1 <varint zigzag>             → signed integer
0xF2 <4 bytes IEEE754>           → float32
0xF3 <8 bytes IEEE754>           → float64
0xF4 <varint index>              → backreference to byte offset
0xF5 <varint count> <token>      → repeat token N times

ADAPTIVE VOCAB (0xC0–0xEF):
Defined per-file in segment header. Decoder loads table before decoding body.

RULES:
- Containers must balance: { } [ ] ( ) < >
- Whitespace implicit unless format requires it (then use 0x0F for \n)
- Repetition can nest: 0xF5 10 0xF5 5 0x21 → "1" repeated 50 times
```

**Usage example prompt**:
```
You are an encoder. Given JSON input, output Janus Notation tokens (hex).
Grammar: [paste 2 KB reference above]

Input: {"x":10}
Output: 0x03 0x40 0x07 0xF1 0x14 0x04
```

LLMs can learn this via:
- Few-shot examples in prompt (grammar + 3–5 encode/decode examples)
- Fine-tuning on synthetic corpus of (original, notation) pairs

**Realistic constraint**: An LLM cannot generate valid Janus Notation without the grammar in context or fine-tuning. This is by design — the language is formal and constructed, not natural.

---

## 8. Phased Roadmap Integration

### Phase 1: MVP (Current — already complete)
- ✅ Brotli/LZMA hybrid engine
- ✅ RLE/Delta prefilters
- ✅ Multi-segment `.jan` container
- ✅ Windows build working

### Phase 2: Janus Notation Core (Next — 3–4 weeks)

#### Module 2.1: Grammar Implementation
**Task**: Implement token encoder/decoder for core vocabulary (0x00–0xBF) and literal types (0xF0–0xF5).

**Inputs**:
- This specification document (sections 2.1, 2.2, 2.4)
- Reference table of 192 fixed opcodes

**Outputs**:
- `notation_tokens.hpp` (token definitions, varint encoding/decoding)
- `notation_encoder.cpp` (stateful encoder: feed bytes, emit tokens)
- `notation_decoder.cpp` (stateful decoder: feed tokens, emit bytes)

**Constraints**:
- No inline comments in implementation code. Function/variable names must be self-documenting.
- Must handle malformed input gracefully (return error, do not crash).

**Verification**: Unit tests for each token type (encode/decode round-trip).

---

#### Module 2.2: Adaptive Vocabulary System
**Task**: Build frequency analysis and adaptive token assignment (0xC0–0xEF).

**Inputs**:
- Input data buffer (from file or stream)
- Max adaptive tokens (default: 48)

**Outputs**:
- Adaptive vocab table (binary format: count + list of (opcode, string) pairs)
- Modified encoder that uses adaptive tokens when available

**Algorithm** (high-level):
1. Scan input, count frequency of all multi-byte tokens (identifiers, keys, literals) not in core vocab.
2. Sort by frequency descending.
3. Assign top N (≤48) to opcodes 0xC0–0xEF.
4. Serialize table to segment header.

**Constraints**:
- No inline comments in implementation code.
- Frequency analysis must complete in O(n) time (single pass, hash map).

**Verification**: Test on JSON file with repeated keys → verify adaptive tokens reduce output size.

---

#### Module 2.3: Container Format Integration
**Task**: Extend `.jan` segment header to support `prefilter_id = 3` (Janus Notation).

**Inputs**:
- Existing `janus_engine.hpp`, `stream_io.hpp`
- Notation encoder/decoder from modules 2.1–2.2

**Outputs**:
- Modified segment header with `encoding_metadata_length` and `encoding_metadata` fields
- Encoder pipeline: data → notation encode → Brotli/LZMA compress
- Decoder pipeline: Brotli/LZMA decompress → notation decode → data

**Constraints**:
- No inline comments in implementation code.
- Must remain backward-compatible with `prefilter_id = 0/1/2` (no notation).

**Verification**: Compress a JSON file with notation enabled, decompress, byte-compare original.

---

### Phase 3: Standalone Library (`libjanus_notation`) — 2 weeks

#### Module 3.1: Library API Surface
**Task**: Refactor notation encoder/decoder into standalone library with public API.

**Inputs**:
- Modules 2.1–2.3 (notation core implementation)
- API spec from section 5.1

**Outputs**:
- `janus_notation.h` (public API header, no internal details exposed)
- `libjanus_notation.a` (static library, Linux/Windows)
- `libjanus_notation.so` / `janus_notation.dll` (dynamic library)
- `Makefile` target: `make libjanus_notation`

**Constraints**:
- No inline comments in implementation code.
- Zero dependencies (STL only, no Brotli/LZMA in this library).
- Thread-safe: encoder/decoder instances can be used concurrently on different threads.

**Verification**: Write minimal example program (`examples/encode_decode.cpp`) that links against the library and encodes/decodes a JSON string.

---

#### Module 3.2: Library Packaging and Documentation
**Task**: Prepare library for redistribution.

**Inputs**:
- Compiled library from module 3.1
- Grammar specification (section 2)

**Outputs**:
- `janus_notation_spec.md` (2–4 KB grammar reference, formatted for LLM context)
- `LIBRARY_README.md` (installation, API usage, integration examples)
- `examples/` directory with 3 sample programs:
  1. Encode JSON to notation
  2. Decode notation to JSON
  3. Stream large file through encoder

**Constraints**:
- Documentation must be concise (no tutorial fluff, just facts).
- Grammar reference must fit in ≤4 KB plain text.

**Verification**: Third-party developer can:
1. Download library + header
2. Read `LIBRARY_README.md`
3. Compile `examples/encode_decode.cpp` with no errors
4. Run and get correct output

---

### Phase 4: Test Suites — 2 weeks

#### Module 4.1: Integrity Test Suite (Test Suite A)
**Task**: Implement automated byte-exact round-trip verification.

**Inputs**:
- Test corpus (minimum 50 files, see section 6.1)
- Full Janus pipeline (notation + compression)

**Outputs**:
- `test_integrity.cpp` (test runner)
- `Makefile` target: `make test_integrity`
- CI config (`.github/workflows/integrity_test.yml`)

**Test logic** (per file):
1. Load file → `original`
2. Compress with Janus (notation + Brotli/LZMA) → `.jan` archive
3. Decompress → `reconstructed`
4. Byte-compare: `memcmp(original, reconstructed, size) == 0`
5. If mismatch: report file, byte offset, segment, hex context

**Pass criterion**: 100% pass rate. Single failure → entire suite fails.

**Constraints**:
- No inline comments in implementation code.
- Must test all edge cases listed in section 6.1 (empty file, huge file, random data, etc.).

**Verification**: Introduce deliberate mismatch (flip one bit in decoder) → test suite must fail with correct offset.

---

#### Module 4.2: Compression Efficiency Test Suite (Test Suite B)
**Task**: Benchmark compression ratios and throughput.

**Inputs**:
- Same test corpus as module 4.1
- Two compressor configs: (1) LZMA-only baseline, (2) Full Janus pipeline

**Outputs**:
- `test_efficiency.cpp` (benchmark runner)
- `Makefile` target: `make test_efficiency`
- Output report: markdown table + CSV (see section 6.2 format)

**Metrics per file**:
- Compression ratio (3 decimal places)
- Encode time (milliseconds)
- Decode time (milliseconds)
- Throughput (MB/s)

**Comparison**: Compute delta (%) between baseline and Janus for each file and category average.

**Constraints**:
- No inline comments in implementation code.
- Must run both configs on identical hardware (no config-dependent optimizations).
- Timing must exclude I/O (measure compression/decompression only).

**Verification**: Run on known file (e.g., 1 MB JSON with repeated keys) → verify Janus ratio > baseline ratio by ≥30%.

---

### Phase 5: Documentation and Release — 1 week

#### Module 5.1: User Documentation
**Task**: Write user-facing guide for Janus Notation.

**Outputs**:
- `NOTATION_GUIDE.md` (for end users of the Janus archiver)
  - What is Janus Notation
  - When to enable it (`-n` flag?)
  - Performance expectations per file type
  - Limitations (not beneficial for already-compressed data)
- Update main `README.md` with notation overview

**Constraints**:
- Keep concise (≤2 pages).
- No marketing language ("revolutionary," "cutting-edge") — factual only.

---

#### Module 5.2: Developer Documentation
**Task**: Write integration guide for third-party developers.

**Outputs**:
- `INTEGRATION.md` (how to embed `libjanus_notation` in another C++ project)
- API reference (generated from `janus_notation.h` docstrings)
- Grammar specification for LLM context (`janus_notation_spec.md`, already in module 3.2)

**Constraints**:
- Must include linking instructions for static/dynamic library on Windows/Linux.
- Must include minimal code example (≤30 lines).

---

#### Module 5.3: GitHub Release Preparation
**Task**: Package binaries and documentation for initial release.

**Outputs**:
- `janus.exe` (Windows, statically linked)
- `janus` (Linux x86_64, statically linked)
- `libjanus_notation.a` + `janus_notation.h` (library package)
- Source tarball (`janus-1.0.0.tar.gz`)
- `CHANGELOG.md` (initial release notes)

**Release checklist**:
- ✅ All tests pass (integrity + efficiency)
- ✅ Builds on Windows (MinGW) and Linux (GCC/Clang)
- ✅ Documentation complete
- ✅ LICENSE file present (MIT or Apache 2.0 recommended)
- ✅ README references library as standalone component

---

## 9. Design Rationale Summary

**Why a formal language instead of just a compression trick?**
- **Reproducibility**: If the grammar is public and fixed, anyone can implement an encoder/decoder that interoperates with Janus.
- **Debuggability**: Notation can be inspected as intermediate representation (like LLVM IR or JVM bytecode).
- **LLM integration**: With a compact grammar (≤4 KB), an LLM can encode/decode directly in prompt context, enabling AI-assisted compression workflows.

**Why fixed core vocab + adaptive per-file vocab?**
- Fixed vocab ensures zero-shot usability (no training needed for common structures).
- Adaptive vocab captures domain-specific repetition (function names in C++, column names in CSV, etc.).
- Hybrid approach balances generality vs. specialization.

**Why standalone library?**
- Decouples notation from archiver — developers can use notation for in-memory structured data compression without touching `.jan` files.
- Enables embedding in other tools (databases, serialization libraries, network protocols).

**Why two separate test suites?**
- Integrity (zero data loss) is non-negotiable — must be 100%.
- Efficiency (compression ratio) is a quality metric, not a correctness metric — allows regression tracking without blocking releases if ratio drops slightly.

---

## 10. Open Questions for Implementation Phase

1. **Whitespace handling**: Should Janus Notation preserve exact whitespace for all formats, or only for whitespace-sensitive languages (Python, YAML)? Proposal: add a `whitespace_mode` flag (PRESERVE / NORMALIZE / IGNORE) to `NotationConfig`.

2. **Streaming API**: Should the encoder/decoder support streaming (incremental feed, incremental output) or only batch mode? Proposal: both — batch API for small files, streaming API for large files (necessary for 100 MB+ test case).

3. **Error recovery**: If the decoder encounters an invalid token mid-stream, should it attempt recovery (skip to next structural boundary) or fail immediately? Proposal: fail immediately (safety-first), add optional recovery mode later.

4. **Versioning**: Should the notation format include a version byte in the segment header? Proposal: yes — reserve 1 byte for format version (currently 0x01), future-proofs against breaking changes.

5. **LLM training corpus**: Should the project include a synthetic dataset of (original, notation) pairs for fine-tuning LLMs? Proposal: defer to Phase 6 (post-release), but document the grammar format to enable third-party training.

---

## 11. Success Criteria

**Janus Notation is considered successful if:**

1. **Integrity**: 100% pass rate on round-trip tests across 50+ diverse files (text, binary, edge cases).
2. **Efficiency**: Average compression ratio improvement ≥30% on structured text (JSON, code, config) vs. LZMA-only baseline.
3. **Usability**: Third-party developer can integrate `libjanus_notation` in <30 minutes with only `LIBRARY_README.md` (no support needed).
4. **LLM-readiness**: Grammar reference fits in ≤4 KB, enabling few-shot encoding/decoding in GPT-4, Claude, or similar models.
5. **Portability**: Compiles and passes all tests on Windows (MinGW), Linux (GCC/Clang), and macOS (Clang) with zero platform-specific patches.

---

**END OF SPECIFICATION**

This document is ready to be handed to a coding model (Kimi Coder) for phased implementation. Each module has explicit inputs, outputs, constraints, and verification criteria. No further architectural decisions are needed.
