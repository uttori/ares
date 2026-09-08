#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"
build_dir=${1:-../../build}
if [ "$#" -gt 0 ]; then shift; fi
configuration=${CONFIGURATION:-Release}
cmake --build "$build_dir" --config "$configuration" --target gdb-polling
ctest --test-dir "$build_dir" -C "$configuration" --output-on-failure -R '^gdb-polling-window$' "$@"
