# Thin wrappers over the CMake presets (CMakePresets.json). `make check` is what every commit must pass.
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)

.PHONY: check asan clean

# Debug build with warnings as errors, then every test.
check:
	cmake --preset dev >/dev/null
	cmake --build --preset dev -j $(JOBS)
	ctest --preset dev -j $(JOBS)

# The same tests under AddressSanitizer and UndefinedBehaviorSanitizer.
asan:
	cmake --preset asan >/dev/null
	cmake --build --preset asan -j $(JOBS)
	ctest --preset asan -j $(JOBS)

clean:
	rm -rf build
