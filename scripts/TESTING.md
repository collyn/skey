# Engine regression tests

## Clang build and diagnostics

Install Clang and its `clang-tools` package (Ubuntu 24.04: `clang-18
clang-tools-18`), alongside the normal CMake, Rust, Fcitx5 and Qt build
dependencies. Run from the repository root:

```sh
CC=clang-18 CXX=clang++-18 CLANG_INCLUDE_CLEANER=clang-include-cleaner-18 \
  bash scripts/check-clang.sh
```

The script uses a separate `build-clang` directory, builds all targets with
compiler warnings treated as errors, runs CTest, then audits unused includes.
It does not install the addon or modify source files. CMake arguments can be
appended, including `-DSKEY_ENGINE_SOURCE_DIR=/path/to/skey-engine` and
`-DSKEY_ENGINE_PREBUILT_LIB=/path/to/libskey_engine.a` for offline builds.
`SKEY_CLANG_BUILD_DIR` and `CMAKE_BUILD_PARALLEL_LEVEL` override the build
directory and job count. Use a new directory when changing compiler versions.

The `Clang checks` GitHub workflow runs this on pull requests and manual
dispatch, retaining the diagnostic log as an artifact. Compile/test failures
and include-tool errors fail the job. Existing unused-include suggestions
are reported for review, not applied automatically or treated as CI failures.
To enforce unused-include checks for a reviewed file:

```sh
python3 scripts/check-includes.py --tool clang-include-cleaner-18 --strict \
  src/test_engine_performance.cpp
```

Omit the filenames to audit all project C++ sources in the compilation
database. This audit checks unused includes only; it is not a clang-tidy
static-analysis pass or a substitute for runtime tests.

## Running specific tests

Using an existing configured build directory:

```sh
cmake --build build -j 4 --target test_engine_performance test_vietnamese
ctest --test-dir build -R 'EnginePerformance|VietnameseTyping' --output-on-failure
```

`EnginePerformance` includes 12 Office/Uinput word cases:

| Dimension | Cases |
| --- | --- |
| Focus-group routing | Wayland, X11 |
| Telex input → expected text | `ddaay` → `đây`, `ddaya` → `đây` |
| Key arrival | After each replacement, burst before the sync anchor, burst during commit settling |

The cases drive the real SKey key handler and capture its Uinput requests
through a private Unix socket pair. They feed back the requested Backspaces,
check that the sync anchor is consumed, and assert the application model's
UTF-8 text, deletion count, and drained transaction queues. The Wayland model
deliberately allows a raw append to overtake pending IM commits; removing the
Office commit-append fix must fail this regression. The X11 model uses ordered
delivery and checks that the existing raw-append route still works.

Both routing modes run in one process without a desktop, LibreOffice, or a
real Uinput server. No keys are injected into other applications. The tests
require permission to send on a local Unix socket pair; sandboxes blocking
that operation fail with `EPERM` rather than skip the tests.

This is a deterministic engine regression, not proof of compositor or
LibreOffice toolkit behavior. Settle boundaries are advanced explicitly;
the existing timer/cancellation tests cover the actual event-loop scheduling.
Real integration validation still requires typing in LibreOffice in both
desktop sessions and comparing the document text. The older `test_e2e.*`
log checks alone cannot establish that the document received the right text.

Firefox X11 regressions also cover `gox` → `gõ` with valid, matching
surrounding text and a reset after each key, without the Url capability.
The application model verifies that the native delete removes `o` before
committing `õ`, and that no Uinput request is sent. Separate cases retain
the Uinput fallback for missing/stale snapshots and Docs-suite editors.
