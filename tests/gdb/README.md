# Native GDB regression checks

The C++ checks compile actual server/core sources, use the ares CMake build,
and run through CTest. No Node.js, private trace packets, or commercial ROMs
are needed. Enable `ARES_BUILD_OPTIONAL_TARGETS` with `sfc` in `ARES_CORES`:

```sh
cmake -S . -B build -DARES_BUILD_OPTIONAL_TARGETS=ON -DARES_CORES=sfc
tests/gdb/run-tests.sh /absolute/path/to/build
```

The shell launcher builds `gdb-tests` and runs the matching CTest cases. Additional
CTest arguments follow the build-directory argument, for example `--verbose`.
For multi-configuration generators, `CONFIGURATION=Debug` selects the same build
and test configuration (default: Release). Compiler selection, include
paths, platform libraries, and the core profile come from the normal ares build.

- `gdb-registers` checks 65816 register layout and normalization with actual
  WDC65816 register types and a register-only CPU fixture.
- `gdb-memory-packets` rejects malformed, overflowing, and oversized RSP memory
  requests before invoking storage.
- `gdb-storage` links the real SFC core and checks WRAM aliases, device-handler
  exclusion, cartridge RAM offsets, remap/unmap, and physical storage bounds.
- `gdb-boundaries` checks deferred stop/reset responses, stepping off breakpoints,
  quiescent stops, disconnect/reconnect, and legacy-core behavior.

Only the test translation units use `-fno-access-control` to inspect protocol
state; assertions remain enabled in release builds with `-UNDEBUG`. The ares
Clang/GCC toolchains are supported. This preparation was run on macOS arm64;
Linux/Windows execution remains before runtime promotion.

The register contract targets 65816-aware RSP clients. Stock GDB has no 65816
architecture backend, and XML alone does not add one.

The core reports instruction boundaries and idle boundaries within WAI/STP.
Idle boundaries permit control without firing an execution breakpoint at the
following PC. `monitor reset`/`reset halt` complete at the reset-vector boundary;
`monitor reset run` preserves breakpoints and resumes subject to desktop pause.
An RSP control loop must continue entering the core while a stop is pending.
