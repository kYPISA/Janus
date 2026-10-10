#!/usr/bin/env python3
"""Build executable, shared library and tests through the canonical CMake build."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "native")
    parser.add_argument("--generator", help="CMake generator (e.g. MinGW Makefiles)")
    parser.add_argument("--toolchain", type=Path, help="Cross-build CMake toolchain")
    parser.add_argument("--no-test", action="store_true")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 4))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    cmake = shutil.which("cmake")
    ctest = shutil.which("ctest")
    if not cmake or (not args.no_test and not ctest):
        parser.error("Install CMake (e.g. python -m pip install cmake) and add its bin directory to PATH")
    build = args.build_dir.resolve()
    command = [cmake, "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
               "-DBUILD_TESTING=" + ("OFF" if args.no_test else "ON")]
    if args.generator:
        command += ["-G", args.generator]
    if args.toolchain:
        if not args.no_test:
            parser.error("Cross-builds require --no-test; run native tests on the target OS")
        command += ["-DCMAKE_TOOLCHAIN_FILE=" + str(args.toolchain.resolve())]
    subprocess.run(command, check=True)
    subprocess.run([cmake, "--build", str(build), "--config", "Release", "--parallel", str(args.jobs)], check=True)
    if not args.no_test:
        subprocess.run([ctest, "--test-dir", str(build), "-C", "Release", "--output-on-failure"], check=True)
    print(f"Artifacts: {build / 'bin'} and {build / 'lib'} (Release subfolder for multi-config generators)")


if __name__ == "__main__":
    main()