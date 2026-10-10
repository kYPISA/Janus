"""Deterministic CLI / shared-library round trips and hostile-archive regressions."""
import argparse
import concurrent.futures
import ctypes
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest
import zlib

parser = argparse.ArgumentParser()
parser.add_argument("--exe", required=True)
parser.add_argument("--library", required=True)
args, unittest_args = parser.parse_known_args()
EXE = str(Path(args.exe).resolve())
LIB = ctypes.CDLL(str(Path(args.library).resolve()))
LIB.janus_compress_file.argtypes = [ctypes.c_char_p] * 3 + [ctypes.c_int]
LIB.janus_compress_file.restype = ctypes.c_int
LIB.janus_decompress_file.argtypes = [ctypes.c_char_p] * 3
LIB.janus_decompress_file.restype = ctypes.c_int
LIB.janus_last_error.restype = ctypes.c_char_p


def varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def archive(payload, original, algo=0, prefilter=0, version=3, crc=None):
    checksum = zlib.crc32(original) if crc is None else crc
    header = struct.pack("<IBBH", 0x53554E41, version, 2, 1)
    if version == 2:
        return header + struct.pack("<QQBBQQI", len(original), 48, algo, prefilter,
                                    len(payload), len(original), checksum) + payload
    return header + struct.pack("<BBIII", algo, prefilter, len(payload), len(original), checksum) + payload


class Integration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="janus-test-")
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *argv, ok=True):
        if argv and argv[0] in ("c", "d"):
            argv = ("-" + argv[0], *argv[1:])
        result = subprocess.run([EXE, *map(str, argv)], capture_output=True, timeout=90)
        diagnostics = result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace")
        self.assertNotIn("AddressSanitizer", diagnostics, diagnostics)
        self.assertNotIn("runtime error:", diagnostics, diagnostics)
        if ok:
            self.assertEqual(result.returncode, 0, diagnostics)
        else:
            self.assertEqual(result.returncode, 1, "Unexpected success or crash: " + diagnostics)
        return result

    def decode(self, data, expected=None):
        source, dest = self.root / "archive.janus", self.root / "decoded"
        source.write_bytes(data)
        sentinel = b"keep existing output"
        dest.write_bytes(sentinel)
        self.run_cli("d", source, dest, ok=expected is not None)
        self.assertEqual(dest.read_bytes(), sentinel if expected is None else expected)
        self.assertFalse(list(self.root.glob("*.janus-tmp-*")))

    def test_round_trips(self):
        rng = random.Random(51)
        header = b'{"tensor":{"dtype":"F32","shape":[256],"data_offsets":[0,1024]}}'
        cases = {
            "empty.bin": b"", "one.bin": b"X", "random.bin": rng.randbytes(8192),
            "runs.bin": b"A" * 4096 + b"B" * 4096,
            "delta.bin": bytes(range(256)) * 128,
            "data.json": b'{"name":"repeated_identifier","value":123,"data":true}\n' * 1024,
            "model.safetensors": struct.pack("<Q", len(header)) + header + rng.randbytes(1024),
            "model.gguf": b"GGUF" + rng.randbytes(8192),
        }
        for name, data in cases.items():
            with self.subTest(name=name):
                source = self.root / name
                packed, dest = self.root / (name + ".janus"), self.root / (name + ".out")
                source.write_bytes(data)
                self.run_cli("c", source, packed, "--threads", 2)
                self.run_cli("d", packed, dest)
                self.assertEqual(data, dest.read_bytes())

    def test_multisegment(self):
        source, packed, dest = [self.root / n for n in ("big.bin", "big.janus", "big.out")]
        data = random.Random(20).randbytes(8 * 1024 * 1024 + 17)
        source.write_bytes(data)
        self.run_cli("c", source, packed, "--threads", 2)
        self.assertGreater(struct.unpack_from("<H", packed.read_bytes(), 6)[0], 1)
        self.run_cli("d", packed, dest)
        self.assertEqual(data, dest.read_bytes())

    def test_raw_round_trips(self):
        for mode in ("--raw-brotli", "--raw-lzma"):
            # Longer periodic input exercises LZMA's long-match relative offsets.
            for data in (b"", b"raw codec test" * 50, bytes(range(256)) * 32):
                with self.subTest(mode=mode, size=len(data)):
                    source, packed, dest = [self.root / n for n in ("raw.in", "raw.packed", "raw.out")]
                    source.write_bytes(data)
                    self.run_cli(mode, source, packed)
                    self.run_cli(mode, "-d", packed, dest)
                    self.assertEqual(data, dest.read_bytes())
                    malformed = bytearray(packed.read_bytes())
                    struct.pack_into("<Q", malformed, 5 if mode == "--raw-lzma" else 0, 0xFFFFFFFFFFFFFFFF)
                    packed.write_bytes(malformed)
                    dest.write_bytes(b"keep")
                    self.run_cli(mode, "-d", packed, dest, ok=False)
                    self.assertEqual(dest.read_bytes(), b"keep")

    def test_segment_boundary(self):
        chunk = 8 * 1024 * 1024
        data = b"\0" * chunk + b"x"
        source = self.root / "boundary.bin"
        source.write_bytes(data)
        for threads in (1, 2):
            with self.subTest(threads=threads):
                packed = self.root / f"boundary-{threads}.janus"
                dest = self.root / f"boundary-{threads}.out"
                log = self.root / f"boundary-{threads}.json"
                log.write_bytes(b"replace this log")
                self.run_cli("c", source, packed, "--threads", threads, "--log", log)
                encoded = packed.read_bytes()
                magic, version, _, count = struct.unpack_from("<IBBH", encoded)
                self.assertEqual((magic, version, count), (0x53554E41, 3, 2))
                sizes = [struct.unpack_from("<BBIII", encoded, 8 + 14 * i)[3] for i in range(count)]
                self.assertEqual(len(sizes), 2)
                self.assertTrue(all(0 < size <= chunk for size in sizes))
                self.assertEqual(sum(sizes), len(data))
                self.assertEqual([s["original_size"] for s in json.loads(log.read_text())["segments"]], sizes)
                self.run_cli("d", packed, dest, "--threads", threads)
                self.assertEqual(dest.read_bytes(), data)
        self.assertFalse(list(self.root.glob("*.janus-tmp-*")))

    def test_failed_decode_preserves_log(self):
        packed, dest, log = [self.root / name for name in ("bad.janus", "out", "log.json")]
        packed.write_bytes(archive(b"hello", b"hello", crc=0))
        dest.write_bytes(b"keep output")
        log.write_bytes(b"keep log")
        self.run_cli("d", packed, dest, "--log", log, ok=False)
        self.assertEqual(dest.read_bytes(), b"keep output")
        self.assertEqual(log.read_bytes(), b"keep log")
        self.assertFalse(list(self.root.glob("*.janus-tmp-*")))

    def test_legacy_and_filters(self):
        self.decode(archive(b"hello", b"hello", version=2), b"hello")
        self.decode(archive(bytes([65, 1, 1]), b"ABC", prefilter=2), b"ABC")
        self.decode(archive(bytes([3, 65, 2, 66]), b"AAABB", prefilter=1), b"AAABB")
        notation = varint(0) + varint(3) + b"abc"
        self.decode(archive(notation, b"abc", prefilter=3), b"abc")

    def test_malformed(self):
        valid = archive(b"hello", b"hello")
        bad = [valid[:i] for i in range(len(valid))]
        bad += [valid + b"extra", archive(b"hello", b"hello", algo=255),
                archive(b"hello", b"hello", prefilter=255), archive(b"x", b"xx"),
                archive(b"hello", b"hello", crc=0), archive(b"\x01", b"A", prefilter=1),
                archive(b"\x00A", b"A", prefilter=1), archive(b"\xffA", b"A", prefilter=1)]
        oversized = bytearray(valid)
        struct.pack_into("<I", oversized, 14, 0xFFFFFFFF)
        bad.append(oversized)
        for payload in [b"\xff\xff\xff\xff\x1f", varint(0) + varint(0xFFFFFFFF) + b"x",
                        varint(0) + varint(6) + b"\xFE\xff\xff\xff\xff\x0f",
                        varint(0) + varint(2) + b"\xF0\x05"]:
            bad.append(archive(payload, b"x", prefilter=3))
        for index, data in enumerate(bad):
            with self.subTest(index=index):
                self.decode(data)

    def test_paths_and_arguments(self):
        source = self.root / "input"
        source.write_bytes(b"keep")
        self.run_cli("c", source, source, ok=False)
        self.assertEqual(source.read_bytes(), b"keep")
        for value in ["-1", "0", "17", "bad", "2x"]:
            self.run_cli("c", source, self.root / "out", "--threads", value, ok=False)
        self.run_cli("c", source, self.root / "out", "--log", source, ok=False)
        alias = self.root / "alias"
        try:
            os.link(source, alias)
        except OSError:
            pass
        else:
            self.run_cli("c", source, alias, ok=False)
            self.assertEqual(source.read_bytes(), b"keep")

    def test_shared_api_and_threads(self):
        self.assertEqual(LIB.janus_compress_file(None, b"unused", None, 0), 0)
        self.assertTrue(LIB.janus_last_error())

        def roundtrip(index):
            source, packed, dest = [self.root / f"api-{index}.{ext}" for ext in ("in", "janus", "out")]
            data = random.Random(index).randbytes(128)
            source.write_bytes(data)
            encode = lambda p: os.fsencode(p)
            self.assertEqual(LIB.janus_compress_file(encode(source), encode(packed), None, 1), 1, LIB.janus_last_error())
            self.assertEqual(LIB.janus_decompress_file(encode(packed), encode(dest), None), 1, LIB.janus_last_error())
            self.assertEqual(data, dest.read_bytes())
            self.assertEqual(LIB.janus_last_error(), b"")

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(roundtrip, range(4)))


if __name__ == "__main__":
    unittest.main(argv=[__file__, *unittest_args])