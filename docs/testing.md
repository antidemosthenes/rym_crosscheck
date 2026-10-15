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

## Registered CTest checks

| Check | What it runs | Coverage |
| --- | --- | --- |
| `rym-correctness` | `rym-tests` | Matching, parsing, acquisition fixtures, storage, scanning, offline CLI behavior, and resource lifecycle checks. |
| `rym-cli-help` | `rym-crosscheck --help` | The CLI can display help successfully without a music directory. |

The correctness executable uses synthetic data, reduced HTML fixtures, and temporary test directories. It does not access RYM, open Edge, or read your music library. It checks:

- Unicode normalization, remaster/version handling, exact ownership, collaboration credits, duplicate editions, and threshold boundaries.
- Indexed suggestions against exhaustive scoring, plus bounded Levenshtein distance against an independent dynamic-programming reference.
- Chart/song HTML parsing, fixture-based pagination, and acquisition failures.
- Album worker limits, exactly-once job assignment, job/result association, and stopping paced requests through a cancellation token. Security-page detection includes RYM's own verification gate.
- Tag priority, filename fallback, tag-cache reuse/invalidation, whitelist validation, and offline snapshot completeness.
- Cache age/size limits, cancellation, cache locking, atomic reporting, and previous-report preservation.
- Resource moves, CLI worker-range validation and removed-option rejection without cache/report changes, and repeated-run handle stability.

Debug tests additionally check CRT allocation differences across warmed repeated runs. These are targeted regression checks, not a guarantee about every possible allocation or failure path.

Upstream dependency test suites are disabled in the dependency configuration, independently of `RYM_BUILD_TESTS`.

## Release helper checks

Run these separately from CTest, from the repository root:

```powershell
./tests/release.Tests.ps1
```

These PowerShell checks cover version ordering, first-release numbering, retry tag reuse, draft completion, published-release preservation, and conflicting-tag rejection. GitHub responses are simulated; the checks make no network requests and do not publish anything. The [release workflow](releases.md) builds without test targets and skips both CTest and these helper checks to minimize release build time.

## Optional live browser-visibility check

`rym-browser-visibility` is built when project tests are enabled, but is not registered with CTest. It needs internet access, Edge, and a previously verified dedicated profile. First complete a normal live CLI comparison using that profile; then close the run before invoking the check:

```powershell
./build/release/rym-browser-visibility.exe 'PATH/TO/DEDICATED/PROFILE'
```

The check exercises chart pagination, parallel song retrieval, a browser move, and teardown while monitoring for new visible Edge windows. It fails if retrieval opens one, access requires verification, or usable content cannot be retrieved. It does not perform the visible verification handoff itself. Avoid opening unrelated Edge windows during the check because it monitors newly visible Edge windows globally.

This live check does not write the application's missing-song report. Access failures and expired cookies can affect its result; see [runtime troubleshooting](runtime.md#troubleshooting).
