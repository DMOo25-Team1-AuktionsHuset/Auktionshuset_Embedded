#!/usr/bin/env bash
# Run from the embedded repository root, or pass its path as the first argument.
set -eu
root="${1:-.}"
tests="$root/HardwareTest/AuctionDeviceTests"
out="$tests/sketch_test_runner.exe"
trap 'rm -f "$out"' EXIT
g++ -std=c++17 -Wall -Wextra -I"$tests/stubs" "$tests/sketch_test.cpp" -o "$out"
"$out"
