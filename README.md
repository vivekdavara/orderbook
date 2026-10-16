# Orderbook

A limit order book and matching engine in C++20: price-time priority, with limit, market, immediate-or-cancel
(IOC) and cancel orders.

**Status: day 1 of 5, in progress.** The CMake scaffold and the shared types are in; the design, the reference
model and the test and benchmark harness are being built today.

## Build and test

Needs CMake 3.25 or newer, Ninja, and a C++20 compiler (Apple clang 21 and GCC 13 are what CI and this Mac use).
GoogleTest is fetched at configure time, pinned by SHA-256.

```bash
make check
```

That configures the `dev` preset (Debug, warnings as errors) into `build/dev`, builds, and runs every test with
CTest. `make asan` runs the same tests under AddressSanitizer and UndefinedBehaviorSanitizer.

## License

MIT
