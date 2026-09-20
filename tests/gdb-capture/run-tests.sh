#!/usr/bin/env sh
set -eu

# Use the caller's configured ares build and the same configuration for CTest.
build_dir=${1:?Usage: run-tests.sh /absolute/path/to/build [ctest arguments]}
shift
configuration=${CONFIGURATION:-Release}
cmake --build "$build_dir" --config "$configuration" --target gdb-capture gdb-capture-bus gdb-states
ctest --test-dir "$build_dir" --build-config "$configuration" -R '^gdb-(capture|states)' --output-on-failure "$@"
