# Compatibility entry points; CMake owns the source list and platform flags.
CMAKE ?= cmake
BUILD_DIR ?= build/native
JOBS ?= 4

.PHONY: all test clean libjanus_notation.a
all:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) --config Release --parallel $(JOBS)

test: all
	ctest --test-dir $(BUILD_DIR) -C Release --output-on-failure

libjanus_notation.a: all
	$(CMAKE) --build $(BUILD_DIR) --config Release --target janus_notation

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean