# GDB polling-window regression

Configure ares with `-DARES_BUILD_OPTIONAL_TARGETS=ON`, then run:

```sh
tests/gdb-polling/run-tests.sh /absolute/path/to/build
```

Like the processor tests, this is a C++ executable built by CMake, with a shell
launcher and CTest registration. It requires no Node.js, ROM, debugger client,
socket, or SNES debugger patches.

CMake extracts the actual `Server::updateLoop()` method from `nall` into a build
include. The test supplies a deterministic transport and counts polls, messages,
acknowledgements, and sleeps. It does not duplicate the polling algorithm or
change the production server to expose a test interface. CMake watches the source
and regenerates the include after edits; extraction fails if the method boundary
cannot be identified.

Checks cover inactive/idle operation, queued traffic, a message at the idle
boundary, the reset guard under continuous traffic, immediate return on resume,
and disconnect with/without acknowledgements. A hard poll limit and CTest timeout
turn an unbounded loop into a failure. Explicit checks remain active in release
builds. Restoring `i = loopCount` makes five of the eleven scenarios fail.

This checks scheduling and fairness. Packet parsing and desktop/client
interoperability are separate integration checks.
