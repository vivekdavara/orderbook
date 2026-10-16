# Thin wrappers over the CMake presets (CMakePresets.json). `make check` is what every commit must pass.
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)

.PHONY: check asan bench clean

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

# Optimised build, then every benchmark 5 times; Google Benchmark reports the mean, median, stddev and CV of each.
# BENCH_ARGS passes more flags, e.g. BENCH_ARGS=--benchmark_filter=ReferenceBook.
bench:
	cmake --preset release >/dev/null
	cmake --build --preset release -j $(JOBS) --target orderbook_bench
	./build/release/bench/orderbook_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true $(BENCH_ARGS)

clean:
	rm -rf build
