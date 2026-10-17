# Testing guide

The [build guide](build.md) explains prerequisites, build parallelism, and `RYM_BUILD_TESTS`. The [README](../README.md) describes user-visible behavior; [architecture](architecture.md) explains the matching guarantees these checks protect.

## Building versus running tests

Project test executables are built by default when `RYM_BUILD_TESTS=ON`, in both Release and Debug. Building does not execute tests. From an x64 developer terminal with CMake and Ninja on PATH:

```powershell
cmake --preset release -DRYM_BUILD_TESTS=ON
cmake --build --preset release --parallel 8
ctest --preset release
```

Replace `release` with `debug` for Debug. The supplied helper combines all three steps:

```powershell
./scripts/build.ps1 -Action all -Preset release
./scripts/build.ps1 -Action all -Preset debug
```

Use `-Action test` to run checks that are already built, or `-Action build` to build without executing them. To exclude test targets entirely, follow [building without tests](build.md#build-without-tests); skip CTest for that configuration.

Each helper `all` or `configure` invocation clears `build/` except `build/downloads/`, including other presets and portable runtime data. In the two helper commands above, starting Debug therefore removes the completed Release build. Use direct CMake commands to keep both configurations, or `build`/`test` to reuse a prepared one. See the [cleanup behavior](build.md#configure-build-and-test) before rebuilding a portable installation containing personal data.

## Registered CTest checks

| Check | What it runs | Coverage |
| --- | --- | --- |
| `rym-correctness` | `rym-tests` | Matching, parsing, acquisition fixtures, storage, scanning, offline CLI behavior, portable report/whitelist review, and resource lifecycle checks. |
| `rym-cli-help` | `rym-crosscheck --help` | The CLI can display help successfully without a music directory. |

The correctness executable uses synthetic data, reduced HTML fixtures, and temporary test directories. It does not access RYM, open Edge, or read your music library. It checks:

- Unicode normalization, remaster/version handling, exact ownership, collaboration credits, duplicate editions, and threshold boundaries.
- Indexed suggestions against exhaustive scoring, plus bounded Levenshtein distance against an independent dynamic-programming reference.
- Chart/song HTML parsing, RYM and Cloudflare restriction detection, fixture-based pagination, unrecognized-page deadlines, security/sign-in recovery in either order without losing ranks, bounded retries, failed handoffs, cancellation before navigation, and acquisition failures.
- Album worker limits, exactly-once job assignment, job/result association, and stopping paced requests through a cancellation token. Security-page detection includes RYM's own verification gate.
- Tag priority, filename fallback, tag-cache reuse/invalidation, whitelist validation, and offline snapshot completeness. Invalid cache source/date/null-containing text is rejected, and an invalid offline context preserves the previous report.
- Cache age/size limits, cancellation, cache locking, failed explicit cache clearing, permission-error diagnostics, atomic reporting, and previous-report preservation.
- JSON report round trips, Unicode/hostile metadata, relative candidate paths, source/date validation, schema rejection, and scan context.
- No automatic legacy migration, canonical TOML compatibility, deduplication, authoritative replacement of displayed entries, external-difference detection without selections, newer-generation preservation, malformed input, read-only failures, and partial report-refresh failure.
- Shared installation paths/defaults, remembered settings and transient resets, required-folder validation, Windows command-line quoting, hidden execution and structured verification progress, cumulative chart/local counters across scan-worker updates and progress coalescing, cooperative cancellation, bounded diagnostics, run-lock rejection before writes, and repeated supervisor handle cleanup.
- Independent report/whitelist imports, preview cancellation, source preservation, active-file no-ops, malformed input, permission failures, local report generations, concurrent full-set saves, and recovery when the other active data file is malformed. An unreadable loaded whitelist cannot become an empty replacement.
- Resource moves, CLI worker-range validation and removed-option rejection without cache/report changes, and repeated-run handle stability.
- Browser process stream isolation using a local child executable: null input/output, exclusion of unrelated inheritable handles, unchanged parent error output, and handle cleanup after successful and failed launches. This does not start Edge or contact RYM.
- Loopback WebSocket fixtures for failed handshakes, cancellation during handshake/receive, stalled reads, text fragmentation, peer closure, binary/oversized-message rejection, and asynchronous events that must not restart a command deadline. Warmed repeated successes and failures check handle cleanup and, in Debug, retained CRT allocations. Browser reply tests retain and move parsed JSON beyond its parsing scope. These tests use Windows networking/cryptography APIs and require no Node or external service.

Debug tests additionally check CRT allocation differences across warmed repeated runs. These are targeted regression checks, not a guarantee about every possible allocation or failure path.

Upstream dependency test suites are disabled in the dependency configuration, independently of `RYM_BUILD_TESTS`.

## Optional live browser-visibility check

`rym-browser-visibility` is built when project tests are enabled, but is not registered with CTest. It needs internet access, Edge, and a previously verified dedicated profile. First complete a normal live CLI comparison using that profile; then close the run before invoking the check:

```powershell
./build/release/rym-browser-visibility.exe 'PATH/TO/DEDICATED/PROFILE'
```

The check exercises chart pagination, parallel song retrieval, a browser move, and teardown while monitoring for new visible Edge windows. It fails if retrieval opens one, access requires verification, or usable content cannot be retrieved. It does not perform the visible verification handoff itself. Avoid opening unrelated Edge windows during the check because it monitors newly visible Edge windows globally.

This live check does not write the application's missing-song report. Access failures and expired cookies can affect its result; see [runtime troubleshooting](runtime.md#troubleshooting).

## Native reviewer checks

The correctness suite tests the shared report store and child supervisor without opening WebView2. For native UI verification, use synthetic reports in a disposable portable installation. Check maximized Home, both primary actions, native folder/file pickers, options and remembered settings, Running stages and cancellation, Stop and close/Keep running/Cancel, Retry, and previous results. Verification handoff requires a live run and user-completed verification; close its dedicated Edge window to resume.

Check empty and malformed Results with both Load buttons available, filename/count previews and cancellation, independent JSON/TOML replacement, unchanged sources, and active-file no-ops. Verify the collapsed whitelist, literal hostile titles, 100-row pagination, cached search/status filters, all-page selection and hidden-selection counts. External whitelist edits must not appear automatically: departure offers Replace local/Discard/Cancel even with no selections. Cancel preserves the screen, Discard preserves disk, and Replace writes the displayed set. New reports must pass the same guard without silently losing choices. Check single-window reuse, `--show-results`, minimized restore, and teardown.

An unreadable whitelist must disable checkboxes, bulk selection, and Save while leaving viewing, navigation, and validated loading available. An error while a native operation is still busy must not re-enable its controls. Opening another viewer instance during a native picker or confirmation must defer navigation until that modal closes.

For performance checks, compare identical synthetic 200-, 2,000-, and 10,000-song reports with the prior reviewer: Home startup, Results preparation, filtering, bulk selection, page transitions, process memory, and repeated-run handle/process cleanup. A temporary script harness can exercise the embedded JavaScript without frontend packages; actual layout and renderer measurements require WebView2. Check retained DOM nodes/listeners after repeated report replacement as well as operating-system handles and processes. Measure larger synthetic libraries separately for index construction and candidate scoring. Measurement scripts and generated data are temporary, kept under ignored build directories and removed afterward; they are not project targets or dependencies.

Use a disposable browser profile for fresh live access tests and then reuse it to test persistence. Gate selection is server-controlled: record whether Cloudflare, RYM's reCAPTCHA form, or sign-in actually appeared. No interactive gate means that manual solving was not live-tested. See [fresh-access testing](runtime.md#testing-fresh-access).

The GUI requires WebView2 Evergreen at runtime; ordinary CTest does not. The release workflow continues to build with project tests disabled. Test targets are application correctness checks, not dependency-cache or publishing infrastructure tests.
