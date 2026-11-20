# Repository Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement the tasks. Steps use checkbox syntax for tracking.

**Goal** — Audit the Plasma lyrics widget, preserve public contracts, and ship regression tests and fixes for confirmed defects.

**Architecture** — Separate parsing and track models, provider adapters, applet presentation, search orchestration, and packaging. Run tests through the same parser, provider, applet, and installer entry points used in production.

**Tech stack** — C++17, Qt 6.5+, KDE Frameworks 6, Plasma 6, QML, CMake 3.20+, CTest, Ninja, Bash.

**Spec** — The repository audit request defines the scope. Existing interfaces and compatibility expectations are in the README, source, and tests.

## Global constraints

- Preserve provider IDs and interfaces, QML properties and signals, settings keys, supported lyric formats, and installer command-line options.
- For each behavior fix, add a focused test first, observe the expected failure, fix the cause, and rerun the same test. Add characterization tests for untested critical paths.
- Use in-memory fixtures, temporary directories, and loopback HTTP. Test security hardening through defensive invariants. Keep test runs independent of live provider uptime and desktop configuration.
- Audit correctness, boundary inputs, ownership and cleanup, trust boundaries, duplication, and measurable or structurally significant hot paths.
- Keep edit ownership disjoint. The integration owner registers any new targets in CMake and handles CI. Cross-review each diff and review the combined diff before final verification.

## Review focus

- Unrepresentable timestamps must not turn into valid timing data.
- Failed, cancelled, and stale provider replies must not contribute current lyrics.
- A media item or position changing during a pending operation must leave the UI on the current item.
- Timed-out or cancelled lookup and candidate work must not revive discarded state.
- Installer failures and arbitrary configuration order must preserve existing user data.

### Task 1: Lyric codecs and track models

**Files** — Own `src/parsers/` and `src/core/`. Extend `tests/codecs-test.cpp`, `tests/ttml-parser-test.cpp`, `tests/provider-match-test.cpp`, `tests/lyricselection-test.cpp`, or `tests/spicy-parser-test.cpp` as applicable.

**Interfaces** — The existing parser entry points output `Lyrics` with millisecond timings and optional limit status. Track matching and metadata normalization serve the applet and provider adapters.

- [x] Audit numeric conversions, timestamp arithmetic, malformed input, text/sample limits, ordering, normalization, and codec resource lifetime. Check fallback behavior for partially valid files.
- [x] Start with an out-of-range timestamp alongside a valid timed line. Assert that overflow cannot create a false zero-time line or corrupt the valid line. Add precise tests for the other confirmed defects and characterize critical limits or matching rules as needed.
- [x] Run targeted suites with `ctest --test-dir <build> -R 'codecs|ttml-parser|provider-match|lyricselection|spicy-parser' --output-on-failure`. Record the assertion each new reproducer fails.
- [x] Implement minimal checked conversions and arithmetic or other confirmed fixes within the existing interfaces. Consolidate duplicated operations where their contracts match.
- [x] Rerun the same targets, inspect their diff, and supply red/green evidence for cross-review.

### Task 2: Sources, HTTP clients, and caches

**Files** — Own `src/services/` and concrete adapters in `src/providers/`. Extend `tests/lrclib-client-test.cpp`, `tests/spicy-client-test.cpp`, `tests/provider-cache-test.cpp`, or add an adapter integration test. The integration owner handles CMake registration.

**Interfaces** — Preserve `LyricsProvider` signals and the adapter ID, result generation, and cache contracts. The orchestration task owns `LyricsProvider`, `LyricsPipelineManager`, and `CandidateSearchController`.

- [x] Trace local, custom, LRCLIB, Spicy, AMLL, LRCRED, KuGou, NetEase, QQ Music, and Unison inputs through size limits, parsing, cache persistence, redirects, cancellation, and final signals. Inspect authentication/header handling and logging without printing secret values.
- [x] Add an integration case using the existing loopback HTTP helper or a fake backend for each confirmed defect. Assert the terminal result, generation, and cache effect, including isolation of stale operations where relevant. Characterize a critical success path for adapters touched.
- [x] Run the focused provider tests and record expected failures.
- [x] Fix the verified behavior at the adapter or client boundary and remove duplicated response/cleanup logic where behavior can be preserved.
- [x] Rerun all touched provider/client targets and send evidence for cross-review.

### Task 3: Applet, MPRIS, and QML

**Files** — Own `src/applet/`, `contents/`, and `tests/applet-sync-test.cpp`.

**Interfaces** — Preserve MPRIS behavior and the applet's QML API, configuration, lyrics snapshot, and export behavior.

- [x] Follow player selection, generation transitions, playback position, offset, synchronized rendering, import/export, and snapshot expiry. Inspect timers, DBus replies, rich text, URL use, and UI hot paths.
- [x] Use the existing applet test harness to assert playback/track state and snapshot isolation at the relevant transition. Add QML integration assertions for affected UI paths when the applet harness supports them. Prove each confirmed issue red.
- [x] Address root causes and consolidate repeated state transitions only when their invariants agree.
- [x] Rerun `ctest --test-dir <build> -R '^applet-sync$' --output-on-failure` and validate the QML files affected. Supply evidence for cross-review.

### Task 4: Lookup and manual candidate orchestration

**Files** — Own `src/providers/{lyricsprovider,lyricspipelinemanager,candidatesearchcontroller}.*` and `tests/pipeline-test.cpp`. Register sources for tests in `CMakeLists.txt` as needed.

**Interfaces** — Preserve synchronous and asynchronous `LyricsProvider` handlers, generation and status signals, candidate cache, and final result ordering.

- [x] Trace registration, scheduling, cache validity, stale callbacks, timeout, cancellation, ranking, and selection. Verify callers can start another request from a terminal signal.
- [x] Add fake-provider cases for confirmed defects using controlled responses. Assert total emissions, generation, selected result, and post-cancel state as applicable. Add a manual candidate integration case if existing coverage omits a critical path.
- [x] Observe failures with `ctest --test-dir <build> -R '^pipeline$' --output-on-failure` and fix the corresponding state transitions. Keep timer and provider ownership explicit.
- [x] Run affected integration suites and request cross-review.

### Task 5: Packaging, checks, and integration

**Files** — Audit `CMakeLists.txt`, `.github/workflows/`, `metadata.json`, `PKGBUILD`, `plasma-lrc.install`, root scripts, and `scripts/`. Use `tests/packaging-test.sh` if packaging defects need a regression harness.

- [x] Inspect shell error handling, rollback, file ownership and path quoting, package staging, metadata consistency, and release gating. Exercise changes against a temporary prefix and fixture config.
- [x] If uninstall drops or retains incorrect configuration sections, assert an uninstall fixture removes the widget's parent and child groups and preserves a peer applet and a backup. Observe failure before any fix. Use similarly isolated fixtures for other confirmed packaging behavior.
- [x] Cross-review each module and resolve findings. Inspect the combined diff and rerun focused checks after follow-up fixes.
- [x] Configure a clean Release build with `cmake -S . -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release -DPLASMA_LRC_TESTS=ON`, run `cmake --build <build>`, and run `ctest --test-dir <build> --output-on-failure`. Run shell syntax checks, QML lint and available project checks, and stage installation with `DESTDIR` in a temporary directory.
- [x] Report the exact verification results, added coverage, and remaining verification limitations.

## Audit results

| Domain | Changes | Regression coverage |
| --- | --- | --- |
| Codecs and matching | Checked timestamp arithmetic for KRC, QRC and YRC. TTML preserves exact milliseconds, explicit paragraph bounds, untimed text spacing, complete paragraphs in a truncated document and non-inverted word intervals. Static output obeys the same lyric limits. Matching preserves meaningful dash subtitles and handles canonical Unicode equivalents. | `codecs`, `ttml-parser`, `provider-match` and `spicy-parser` cover malformed ranges, fractional timing, partial data, decompression behavior and metadata normalization. |
| Providers and clients | LRCLIB isolates cancelled manual replies and preserves transport and parser errors without negative caching. AMLL and lrc.red preserve cached failure types and reject malformed manual search envelopes. lrc.red, Unison and KuGou propagate parser or reply limits. Spicy keeps unrelated lookups active during invalidation and bounds timestamps. Custom tries all supported lyric fields before reporting failure. Credential-bearing Spicy and Custom requests follow same-origin redirects. | `provider-cache`, `lrclib-client` and `spicy-client` use loopback HTTP and temporary cache directories to cover retries, stale generations, concurrent manual and automatic lookups, redirect behavior, typed failures and successful recovery. |
| Applet and QML | Player locks and MPRIS owner changes detach stale state and refresh invalidated properties. Pause-hide and expiry timers follow settings and the lyrics still on screen. Track changes and terminal lookup results share rendering cleanup and publish row and backing-vocal updates. The tooltip preserves literal metadata and the configuration swatches use supported theme colors and layout sizes. | `applet-sync` checks direct owner replacement, reattachment, paused switching, property invalidation, expiry before the first line and during lookup, lead/backing cleanup and live and snapshot QML behavior. |
| Lookup orchestration | Automatic lookup cancels a selected provider's outstanding upgrade at the deadline and clears prior-attempt rejection details. Manual search retires attempts before aborting, keeps its concurrency bound with synchronous replies, ignores invalidated fetch results and limits stale flags to displayed candidates. | New `candidate-search` suite and extended `pipeline` suite cover abort-time replies, partial and full cache hits, close and refresh reentrancy, queue limits and provisional results. |
| Packaging | Uninstall targets the selected prefix and preserves unrelated and legacy configuration groups. Release rejects an existing tag before changing files. `pkgver` preserves metadata and Git command failures. Build and packaging metadata declare the fixture dependencies. | New `packaging` integration suite covers a prefix containing spaces, install rollback, config backup and preservation, missing package source and an existing release tag in a disposable Git repository. |

The review covered all five domains. Focused reproductions failed before the corresponding behavior fixes. The follow-up review checked TTML, applet rendering cleanup, provider response classification, candidate scheduling and configuration preservation.

## Final verification

- Release build on Linux x86-64 with CMake, Ninja and Qt 6.12. CTest ran all 12 suites with zero failures. A staged CMake installation contained the shared library, exact metadata and QML package.
- The pipeline, candidate search and applet suites each passed three consecutive runs. In the synthetic Release benchmark, the 600-line TTML fixture averaged 7.16 ms before the audit and 6.35 ms after it. The shorter and overlapping-vocal fixtures also completed.
- Bash syntax, ShellCheck, actionlint, JSON and XML parsing, and Git whitespace checks passed.
- The Clang Static Analyzer passed across all 36 source and test translation units, with a fresh targeted run on the final follow-up files. The analyzer database omits the GCC-specific direct-external-access flag. The opt-in virtual-call checker is excluded for the explicit destruction-time cleanup pattern.
- QML lint passed across 13 files with the two runtime-context categories excluded from the warning gate. The full standalone report still contains `unqualified` and `missing-property` diagnostics for context names and properties provided at runtime by Plasma, KConfig and the C++ applet.
- The Release compiler reported deprecated `KLocalizedContext` usage and ignored fixture-file open results in test code.
- Runtime checks used offscreen QML, synthetic MPRIS events, loopback HTTP and staged installation. Live provider behavior, an installed desktop session, and interprocess cache contention were outside these checks.
