# Paused state regression for the staged rich-trace runtime

Enable `ARES_BUILD_OPTIONAL_TARGETS=ON` with `sfc` in `ARES_CORES`, then run
`tests/gdb-states/run-tests.sh /absolute/path/to/build`. The C++ executable links
the actual core and checks repeated save, the undo snapshot before loading,
load, undo load, register/clock stability, retained trace state, and debugger
step/continue control after restoring. Both PPU implementations run through CTest.
Serialization is handed to a separate thread while emulation is idle, as in the
desktop. A bounded event guard catches the original infinite GDB stop loop.

Assertions are enabled after nall's build-mode header. No Node or commercial
ROM is needed. These core tests do not replace the real desktop/socket check.
