#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
build_dir=${1:-../../build}
configuration=${CONFIGURATION:-Release}
cmake --build "$build_dir" --config "$configuration" --target gdb-states
ctest --test-dir "$build_dir" -C "$configuration" --output-on-failure -R '^gdb-states'
