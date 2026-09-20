#!/bin/sh
set -eu
capture_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/ares-capture-file.XXXXXX")
trap 'rm -rf "$capture_test_dir"' EXIT HUP INT TERM
"$1" "$capture_test_dir"
