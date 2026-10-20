# Testing guide

[Build](build.md) explains toolchain requirements and `RYM_BUILD_TESTS`.

## Build and run

From an x64 developer terminal:

```powershell
cmake --preset release -DRYM_BUILD_TESTS=ON
cmake --build --preset release --parallel 8
ctest --preset release
```

Use `debug` in place of `release` for Debug. Direct CMake commands preserve other build directories.

The helper's `all` action configures, builds, and runs CTest.
Its `build` action builds targets without CTest. Its `test` action runs CTest without a build.

**Helper `configure` and `all` remove `build/` contents except downloads.**
This includes other presets and portable data. Read [cleanup rules](build.md#configure-build-and-test) before these actions.

Tests default to ON. Building test executables does not run them.
If test targets are absent, skip CTest. Upstream dependency tests remain disabled independently.

## Automated coverage

| CTest check | Purpose |
| --- | --- |
| `rym-correctness` | Run `rym-tests` with synthetic data and local fixtures. |
| `rym-cli-help` | Check help without a music path. |

Ordinary CTest does not contact RYM, open Edge/WebView2, or read personal music.
Temporary files use disposable test directories.

| Area | Coverage |
| --- | --- |
| Normalization | Unicode, quotes, punctuation, remasters, included sections, recording labels, complete credits, and malformed metadata. |
| Exact ownership | Shared identity keys, strict whitelist pairs, alternate principals in duplicates, edition grouping, and unchanged validation failures. |
| Review evidence | All reported matching examples. Unknown annotations, slash and spaced-dash components, whole-word artist overlap, and combined reasons. |
| Latin spelling | Bôa/BoA, title accents, reordered credits, literal separators, structured-credit collisions, non-Latin preservation, and original-text scores. |
| Title notation | Nujabes example, numbered aliases, featured credits, preserved digits, malformed text, combined rules, original scores, and older reports. |
| Retrieval and scoring | Indexed/exhaustive equality, combined review rules, repeated trigrams, short names, thresholds, shuffled records, long strings, and unlimited edit budgets. |
| Ranking | Exact rational means, overflow-safe products, deterministic ties, complete ranking bounds, and three-candidate capacity. |
| Component storage | More components and deeper nesting than inline capacity. Storage growth must preserve all valid evidence. |
| Reports | JSON round trips, Unicode, hostile metadata, warnings, offline context, relative paths, and optional flags in old reports. |
| Chart access | Pagination, security/sign-in transitions in either order, retry limits, timeout, cancellation, and noninteractive rejection. |
| Album workers | Worker limits, pacing, successful single-attempt jobs, bounded retries, invalid metadata, restrictions, cancellation, and saved metadata warnings. |
| Local storage | Tags, filename format, cache reuse, eviction, invalid snapshots, explicit clear failures, and previous-report preservation. |
| CLI and launcher | Option ranges, removed arguments, paths, settings, transient resets, Windows quoting, run locks, progress, and hidden children. |
| Whitelist and imports | Complete replacement, external differences, malformed files, source preservation, concurrent saves, newer generations, and partial success. |
| Browser transport | Stream isolation, deadlines, failed connections, fragmentation, unrelated events, oversized replies, and repeated handle cleanup. |

A separate dynamic-programming oracle checks edit distance.
The exhaustive matcher bypasses index filters and ranking shortcuts.
These checks test different failure classes.
Dense notation cases check ranking bounds with short and long titles, guest credits, and incompatible recordings.

Debug checks retained CRT allocations across repeated, warmed runs.
These checks cover selected paths. They do not prove that every possible path releases all resources.

## Optional live browser-visibility check

The build includes this executable when `RYM_BUILD_TESTS=ON`.
CTest does not register it.

```powershell
./build/release/rym-browser-visibility.exe 'PATH/TO/DEDICATED/PROFILE'
```

1. Complete a live comparison with that dedicated profile.
2. Close the dedicated Edge window.
3. Close unrelated Edge windows.
4. Run the check.

The check tests pagination, parallel song retrieval, browser moves, and cleanup.
It fails if retrieval opens a visible Edge window or requires verification.
It does not write a comparison report. Session expiry can affect the result.

## Native reviewer checks

1. Prepare a disposable portable installation with synthetic data.
2. Open the reviewer.
3. Check the behavior in the table.
4. Close the reviewer.

| Area | Required behavior |
| --- | --- |
| Startup | Home starts maximized. A second start reuses the same window. |
| Comparison form | Native pickers work. Saved defaults appear. Offline and Clear cache reset. |
| Running | Chart and local counts stay separate. Access instructions remain visible. |
| Cancellation | Cancel requests cleanup. Stop and close waits. Keep running leaves the reviewer open. |
| Failure | Retry and previous results remain available. The previous report stays intact. |
| Empty or malformed data | Results stays accessible. Import controls remain available. |
| Imports | Previews show the filename and count. Cancel and failed writes preserve destinations and sources. |
| Results | Whitelist starts collapsed. Pages contain 100 rows. Filters keep selections across pages. |
| Bulk selection | Select all includes every matching page. Hidden selections remain in counts. |
| Save | Added and removed exclusions move between sections immediately. |
| External edits | Replace local, Discard, and Cancel keep their documented meanings. New reports cannot silently reset choices. |
| Unreadable whitelist | Editing and Save remain disabled. A busy error cannot unlock them. |
| Navigation and links | External links use the normal browser. Minimize/restore and `--show-results` work. |
| Native dialogs | Navigation waits until the open dialog closes. |

Live access tests require a new dedicated profile.
Complete checks and sign-in manually. Reuse the profile to check invisible retrieval.
Record only the gates that appear. [Runtime](runtime.md#testing-fresh-access) explains profile isolation.

## Performance checks

Use identical inputs before and after a change.
Measure index construction, query time, scoring, allocations, memory, and result equality.
Include absent artists, dense title sets, review paths, short names, and long narrow edit bands.

WebView2 checks use 200-, 2,000-, and 10,000-song reports.
Measure startup, loading, filtering, bulk selection, screen changes, memory, and cleanup.
Check retained DOM nodes and listeners after repeated report replacement.

Temporary Node harnesses require no npm packages. Renderer measurements require actual WebView2 execution.
Keep measurement files under ignored build directories. Remove these files after the checks.
Do not add benchmark targets or container/JSON comparisons.

Project tests cover application behavior. CMake-cache and publishing checks are not CTest targets.
GitHub releases build with project tests disabled.
