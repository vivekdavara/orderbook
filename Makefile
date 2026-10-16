# Thin wrappers over the CMake presets (CMakePresets.json). `make check` is what every commit must pass.
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)

.PHONY: check asan bench coverage gcc-check clean

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

# Line, function, region and branch coverage of the library and test kit (src/ and include/) by the GoogleTest suite,
# with Clang's source-based coverage. Each test runs as its own process and writes its own profile; they are merged
# first. (The CLI's tests add nothing in src/ and include/ to it, and their binary's profile doesn't match the test
# binary's for a few library functions, which llvm-cov warns about, so the report reads the test binary alone.)
LLVM_COV ?= $(shell xcrun --find llvm-cov 2>/dev/null || command -v llvm-cov)
LLVM_PROFDATA ?= $(shell xcrun --find llvm-profdata 2>/dev/null || command -v llvm-profdata)
coverage:
	cmake --preset coverage >/dev/null
	cmake --build --preset coverage -j $(JOBS)
	rm -rf build/coverage/profiles
	LLVM_PROFILE_FILE=$(CURDIR)/build/coverage/profiles/%p.profraw ctest --preset coverage -j $(JOBS) >/dev/null
	$(LLVM_PROFDATA) merge -sparse build/coverage/profiles/*.profraw -o build/coverage/tests.profdata
	$(LLVM_COV) report -instr-profile=build/coverage/tests.profdata build/coverage/tests/orderbook_tests \
	  $(CURDIR)/src $(CURDIR)/include

# This project's own sources compiled with GCC (Homebrew's g++-15 here), Debug and Release, warnings as errors:
# CI's GCC job catches warnings Clang doesn't give, and this finds them before a push (scripts/gcc_check.sh).
gcc-check:
	scripts/gcc_check.sh

clean:
	rm -rf build
