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


Chrome X11 Uinput address-bar regressions cover:

- Own-output focus protection expires without being renewed by reactivation;
  moving to another input clears retained composition and reclaim state.
- Distinct nonzero key timestamps preserve deliberate fast repeated keys.
- One ordered queue spans deletion and settle, preserving modifiers, releases
  and timestamps. Escape and sync Backspaces reach the transport handler.
  Replay yields 4ms after replacement and between queued presses; genuine
  focus cleanup cancels the replay timer.
- Suffix replacement preserves unchanged text and tracks the full composed
  length. A pre-word accessibility snapshot cannot justify an extra Backspace
  that deletes the slash before a Vietnamese URL path.

First-word autocomplete regression (Chrome 154.0.8037.57 on X11): Escape
can close the suggestion popup without removing its inline selection. On the
affected test machine, `d`, Escape, Backspace, `Q` produces `dQ` even with
SKey disabled; two Backspaces instead produce `Q`. Thus Escape plus an exact
suffix deletion is not a safe substitute for the full first-word replacement
when accessibility text is unavailable. The existing first-word/prefix guards
still apply; a nonempty, nonmatching URL snapshot retains the suffix fallback.
The transport test models both selected autocomplete consuming the first
Backspace and no selection, where surplus Backspace at the start is harmless.
Before the fix, live `ddaay`, `banj` and `goox` cases retained one old character
(`dđây`, `baạn`, `goỗ`). The updated test fails on the old implementation.

Live Chrome X11 validation used Uinput throughout: 48/48 typing cases,
24/24 latency readbacks, 6/6 localhost Enter cases and 4/4 transitions from
omnibox to textarea passed. The 18 accent samples measured median latency
147.63→79.16ms; nearest-rank p95 was 172.80→169.08ms (the sample maximum).
This is bounded regression evidence, not a guarantee across Chrome versions.
Local Clang and remote GCC builds passed all seven CTests.

Chrome X11 Surrounding Text fallback uses the shared Uinput Backspace anchor
and renderer settle when native surrounding is missing or stale. Regression
cases cover `twj`/`twf` with no capability, an invalid snapshot, a mismatching
snapshot and a matching native snapshot. A delayed loopback must not allow the
old fixed 15ms commit; the real Backspace passes once and the sync key is
consumed before committing the suffix once. Matching native deletion stays
unchanged. These transport tests do not emulate Facebook's renderer.

Repeated-tone Chrome X11 regression covers `chào` → `cháo` → `chạo`, including
both two-character suffix deletions and complete-word length tracking. A fresh
but unclassified a11y node cannot lower the page-editor settle floor to 15ms;
X11 browser page replacements use the 30ms editor floor unless a fresh Sheets
editor is positively identified. Omnibox timing and Wayland policy are unchanged.
This validates routing/timing and emitted text, not acceptance by Facebook's
renderer; the runtime policy log now records freshness/editor classification.

Chrome X11 page replacements also share the Wayland deletion-acknowledgement
and single-character composition-commit policies. Only an exact, newer
surrounding snapshot can acknowledge deletion; the shared 8ms guard and 80ms
bounded timeout apply. Missing/stale initial data retains the renderer floor,
and an unresponsive input disables further ack waits until focus activation.
Manual delay overrides and AutoDelay-off retain their behavior. X11 Sheets
editors, omniboxes (including geometry-only detection) and Electron are excluded
from the new acknowledgement route. Composition is scoped to Unicode single-
character replacements with Preedit support, excludes Sheets/omnibox/Electron,
and leaves no lingering preedit even with Show preedit disabled.

Regression coverage runs Chrome X11 through both direct-key and suffix-replay
paths for `twj`/`twf`: early/late/missing/conflicting acknowledgements, boundary
cancellation, queued navigation, timeout suppression and composition ordering.
These checks validate IME/transport behavior; they do not establish equal
end-to-end latency or renderer reliability between real X11 and Wayland apps.

Tabby/Electron X11 uses the terminal per-deletion settle floor (15ms per
Backspace at fast loopback rates), not the unknown web-editor 30ms floor or
first-focus 50ms floor. Other Electron apps and Wayland retain their policies.
Distinct nonzero X11 timestamps distinguish deliberate repeated letters from
re-delivery. One queue preserves presses, releases and timestamps throughout
replacement. A 20ms event-loop guard after commits prevents the next kernel
Backspace from overtaking Tabby's asynchronous PTY insertion, including keys
that arrive after an initially empty queue. Queued ASCII keys retain their
native identity; single-character IM commits establish composition so Electron
inserts the requested text instead of reusing a newer physical key. Raw unqueued
letters still pass through. Explicit pre-commit overrides retain their values.

Tabby regressions cover one/two/three deletions, first-focus timing, manual
pre-commit overrides, Electron IDE/browser exclusions, unchanged Wayland,
replayed versus distinct timestamps, ordered spaces/releases, short-w append,
late-arriving tone keys, focus cancellation and composition with Show preedit
disabled. Queued ASCII now uses explicit native key forwarding with original
timestamps; spaces and punctuation follow that route too. This avoids repeatedly
starting single-character compositions while Electron still holds old preedit.
Live tests use a dedicated Tabby tab running a raw Python PTY recorder:
input is never evaluated by a shell. Timing is injection-to-PTY receipt, not
screen rendering latency. The same 12-sample-per-case benchmark measured:

| Final key | Median before → after | p95 before → after |
| --- | --- | --- |
| `tw` + `j` → `tự` | 37.02 → 26.24ms | 39.03 → 32.19ms |
| `ban` + `j` → `bạn` | 48.82 → 40.99ms | 56.99 → 46.52ms |
| `go` + `n` (control) | 3.75 → 4.02ms | 5.51 → 6.69ms |

These measurements belong to the intermediate composition-replay candidate:
24/24 typing cases and 36/36 latency readbacks passed, but extended bursts
passed only 119/120 (one duplicated t). The focus-transition run aborted on
an active-window mismatch before completing. The follow-up native ASCII replay
change requires live validation; do not treat these measurements as final
acceptance. Timing samples are small (nearest-rank p95 is the maximum) and
measure this Tabby 1.0.237 X11 machine, not universal latency.

Browser timing on X11 now pays the 50ms first-focus settle once after a
completed Chrome page transaction. Later replacements in that focus retain
the renderer floor (30ms for unknown/page editors; existing Sheets policy)
or exact deletion acknowledgement. Activation and explicit input-boundary
cleanup rearm first-focus settling. Omnibox timing is unchanged.

Chrome/Firefox X11 deletion acknowledgement is notification-driven: one timer
waits for the 80ms fallback deadline, and surrounding-text updates rearm it to
the exact acknowledgement plus the existing 8ms guard. A conflicting snapshot
revokes the acknowledgement and restores the bounded fallback deadline. An
acknowledgement whose guard elapsed before the Backspace anchor does not add
another 8ms wait. Wayland retains its existing polling schedule. Regressions
cover these deadlines, early/late/missing/conflicting observations, cancellation,
manual overrides, one-time focus settling and rearming at input boundaries.

Live browser validation on the X11 machine used isolated localhost textarea
and contenteditable pages, not Facebook's renderer. Chrome's second replacement
soon after page activation (`boo` + `j`) passed 12/12 and improved median
injection-to-DOM latency from 58.66ms to 36.79ms (maximum 66.16→44.62ms).
Warm textarea typing passed 18/18 cases and 30/30 latency readbacks per browser.
Warm Chrome medians were 38.54ms (`twj`) and 38.46ms (`banj`); Firefox medians
were 16.37ms and 31.74ms. These warm samples do not establish a speedup:
baseline medians were 36.21/40.11ms and 14.58/31.16ms respectively. The retained
8ms acknowledgement guard and fallback floors are deliberate. The event-driven
path reduces timer wakeups and avoids an extra guard only when confirmation
precedes the anchor. Clang and remote GCC each passed all seven CTests.
Contenteditable validation passed 18/18 typing cases on each browser, including
`twj`, `twf`, `chafosj` and multi-word Telex at xdotool delay settings 100/60/40ms.
The combined final browser run passed 144/144 checks: 48 textarea checks plus
18 contenteditable checks per browser, and 12 Chrome first-focus measurements.
