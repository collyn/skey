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

Preedit visibility cases run with X11/Wayland focus groups, both with and
without client preedit capability. They cover toggling visibility mid-word,
hidden tone composition and Space commit, changing the display destination,
recovering hidden composition after focus loss, and avoiding duplicate text
or overwriting the mode menu in other output modes.

Chrome Wayland timing tests drive `twj` through the real key handler with an
isolated Uinput transport, a warm 1ms loopback statistic, and a word embedded
inside surrounding text. Fresh, exact post-delete updates arrive after
1, 5, 15, 40 or 70ms. The engine waits until the matching update plus an 8ms
guard before committing; following navigation is queued and replayed once.
The guard is a safety margin, not a renderer commit acknowledgement.

Deterministic timing-policy cases sweep acknowledgement delays from 1–79ms
and cover UTF-8 multi-deletion, stale/wrong text, selection, conflicting
updates, empty results, cancellation and snapshot size bounds. Integration
cases also cover no usable update (80ms deadline), cancellation, and the
buffered-replacement path. A timeout disables acknowledgement waiting until
the next activation, avoiding repeated 80ms waits on an unresponsive input.

Without a trustworthy initial snapshot, the conservative 20/30ms fallback
remains. Explicit pre-commit overrides and disabling AutoDelay bypass this
new observation path. X11, Electron and the browser address bar retain their
existing policies. Tests verify these boundaries and the fallback timer;
they do not emulate Facebook's renderer or prove the real app accepts a
commit. No speculative retry of insertion/deletion is performed.

Chrome Wayland suffix insertion now establishes a transient client composition
before committing a single non-ASCII BMP character after Uinput deletion,
then clears it immediately. This targets Chromium's `NeedInsertChar` /
`MaybeCommitResult` distinction: without composition, a one-code-unit commit
uses `InsertChar`; with composition it uses `InsertText`.
Reference: https://github.com/chromium/chromium/blob/main/ui/base/ime/linux/input_method_auralinux.cc

A scoped regression models an editor ignoring the character-key route and
checks the exact preedit/commit/clear sequence, including Show preedit off.
The real `twj` timer tests also assert composition exists at suffix commit.
Fresh a11y-identified Sheets editors, X11, Electron, omnibox, missing Preedit
capability, and ordinary appends retain
their existing commit path. This transport composition does not hold the word
until Space and introduces no extra timer. It may still produce composition
events in a web editor. The model establishes routing, not that Facebook's
current editor accepts the result: reproduce in a real Chrome Wayland comment
field and check `Uinput: composition commit` alongside the visible result.

Sheets scope regressions ensure a fresh Sheets editor snapshot bypasses the
transient composition workaround; a stale snapshot cannot disable the Facebook
fix. The generic missing-a11y caret fallback no longer logs itself as Sheets
on every key and ignores unavailable (zero-height) caret geometry.

Firefox (including Snap's `firefox` program) on X11 now uses deletion
observation and transient composition for Uinput suffix replacement too.
Url-capable fields and a11y-identified Docs-suite inputs are excluded. The
existing native surrounding-text address-bar route is unchanged; a field
without Url capability that falls back to Uinput may use the new route.
This does not enable the workaround for other Snap applications or Gecko forks.
Firefox GTK distinguishes bare insert-text commands from composition commits:
https://github.com/mozilla/gecko-dev/blob/master/widget/gtk/IMContextWrapper.cpp

Regression cases drive both `twj` and `twf`, inject reset before the first
Backspace and between deletion and sync anchor, and retain the pending word
without tentative surrounding-text reattachment. They cover delayed/missing
delete updates, cell-boundary cancellation, composition at commit, and queued
navigation. These are engine/transport regressions, not a live Snap/Facebook
renderer test. No insertion retry is attempted on a missing acknowledgement.

Firefox X11 idle Backspace passes the original physical key to the application,
without native surrounding deletion or cache mirroring. Ctrl+A may leave a
collapsed surrounding snapshot in GTK; that snapshot must not override the
application's actual selection. Regressions cover Ctrl+A, an intervening reset,
invalid/stale-collapsed/forward/reverse selection snapshots and repeated BS.
Idle Firefox Backspace clears reclaim history, since the engine cannot know
whether the app deleted a separator, a character, or a selection. Active-word
Vietnamese replacement and other applications retain their existing routes.

The attempted zero-guard Firefox optimization was reverted after real-app
regressions. All observation paths retain the 8ms post-delete guard. Firefox
X11 short-w append now arms the same own-output reset guard as forwarded keys;
regressions include a reset after ư while surrounding still reports only t.
