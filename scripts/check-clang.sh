#!/usr/bin/env bash
# Extra arguments are passed to CMake, e.g. -DSKEY_ENGINE_SOURCE_DIR=../skey-engine.
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${SKEY_CLANG_BUILD_DIR:-build-clang}"
cmake -S . -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER="${CC:-clang}" \
    -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DCMAKE_CXX_FLAGS=-Werror \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@"
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
ctest --test-dir "$build_dir" --output-on-failure
python3 scripts/check-includes.py --build-dir "$build_dir" \
    --tool "${CLANG_INCLUDE_CLEANER:-clang-include-cleaner}"
