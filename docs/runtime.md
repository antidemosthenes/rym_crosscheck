# Runtime storage and troubleshooting

See the [README](../README.md) for CLI arguments, verification instructions, whitelist configuration, matching, and report statuses. This guide explains persistent state and recovery.

## Offline comparisons

A successful live comparison saves a chart snapshot in the selected application cache. To compare against that snapshot while rescanning your current library:

```powershell
./build/release/bin/rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --offline --top 200
```

The snapshot must contain complete ranks 1 through the requested N and use a compatible cache schema. A snapshot with more songs can serve a smaller N. Offline mode makes no web requests or album lookups, and the report shows the snapshot's source, capture time, and age. The local library and whitelist are evaluated for the current run.

If the live run used `--cache-dir`, supply the same directory offline. Manual HTML/CSV imports are not supported. An ordinary live run fetches a fresh chart even when a snapshot already exists.

## Application cache

By default, `.rym-crosscheck-cache` in the working directory contains:

| Record | Purpose |
| --- | --- |
| `tracks.json` | Previously read tags, library identity, file paths, sizes, and modification times. Unchanged files reuse tags; changed files are read again. |
| `chart.json` | The most recently saved chart snapshot for offline comparisons. |
| `run.lock` | Prevent concurrent use of the same cache directory. |

Cache records are versioned. Incompatible or unusable tag caches are ignored and tags are rescanned; an unusable chart snapshot prevents an offline comparison. Pruning removes application records older than 30 days and evicts the oldest records when their combined size exceeds 128 MiB. A snapshot can therefore become unavailable through age or size eviction.

`--clear-cache` removes tag and chart records before the requested comparison. Use it with a live run to rebuild the records. Combining it with `--offline` removes the snapshot that offline mode needs. It does not clear browser verification cookies, reports, whitelist files, music, or dependency downloads.

## Browser profiles and cookies

The default dedicated profile is `%LOCALAPPDATA%/rym-crosscheck/edge-profile`. This environment-variable location is independent of the application cache. Override it with `--browser-profile PATH` when necessary, using an application-specific directory. The program rejects a profile directory named `User Data` and symbolic-link profile roots; avoid pointing it at personal browser data.

All live retrieval uses the persistent dedicated Edge profile. Verification and retrieval share its cookies, which stay inside Edge and are retained so later runs may not need another Cloudflare verification. Extensions are disabled for both retrieval and verification. Chart and song collection is invisible; the visible window is reserved for verification as described in the [README](../README.md#what-happens-during-a-run).

The application locks the selected browser profile while using it. Do not run two comparisons with the same persistent profile simultaneously. Separate application caches and browser profiles allow independent runs.

## Reports and cleanup

Album lookups use four workers by default, with a maximum of eight. All workers share the default 1,000 ms interval between page-navigation starts; additional workers overlap slower page loads rather than creating an immediate burst. This is an application limit, not a server-approved quota. Browser asset requests are not counted as separate worker jobs, and RYM can still challenge a run within these settings.

If an album page encounters a security challenge, queued page navigations in that browser session are stopped. Already retrieved albums are retained, and unresolved albums remain `Unknown` with warnings. No automatic retry loop is used to probe the restriction.

Reports are written through a temporary sibling file and replaced atomically. Failed chart retrieval, incomplete chart ranks, or failed verification do not overwrite an existing report. Album lookup failures alone permit reporting with `Unknown` albums. Local scan warnings produce a report with an affected-file list and exit code `2`; see [report interpretation](../README.md#understanding-matches-and-the-report).

On controlled exit, the program joins its workers, releases file/network handles, removes temporary writes, and closes only browser processes it launched. Browser HTTP cache is cleared during retrieval teardown, while persistent verification cookies remain. Source/build files, music, whitelist files, reports, and personal browser data are preserved.

Forced termination can leave temporary files behind. Cache startup removes stale application temporary writes in its own cache directory. Other leftovers are not automatically swept from arbitrary locations.

The default report, cache, personal whitelist, editor settings, and build output are Git-ignored. Custom report, cache, whitelist, or profile locations inside the repository need appropriate ignore rules. Tag caches contain local file paths and metadata; review generated reports before sharing them. Dependency downloads under `build/downloads` are retained for subsequent builds and are not subject to runtime cache pruning.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| Cloudflare verification repeats or expires | Complete verification in the dedicated window, then press Enter in the terminal or close that window. Reuse the same dedicated profile to retain cookies. Increasing `--verification-timeout` gives more time; it does not guarantee access. |
| RYM's security check interrupts album lookup | Let the restriction clear or complete verification when requested on a later run. Reduce `--web-workers` and increase `--request-delay-ms`, for example to `1` worker and `2000` ms. More verification time does not raise a server limit. |
| Verification is required in a noninteractive run | Rerun in an interactive terminal to verify, or use an available offline snapshot. Verification is not automated. |
| Edge cannot start or a browser command fails | Check that a current Edge version is installed, the dedicated profile is writable and unused, and local browser debugging is permitted. |
| The cache or profile is locked | Let the other run finish, or choose a separate cache and dedicated profile. |
| No cached chart, incompatible snapshot, or too few ranks | Complete a live run with at least the desired `--top` value using the same cache directory. |
| The local scan is incomplete | Review the affected-file list, permissions, tags, and filename fallback, then rerun. |
| Whitelist parsing fails | Check the [TOML examples and entry rules](../README.md#customizing-the-whitelist). |
| Chart ranks are incomplete or markup is unsupported | Preserve the previous report and retry later; upstream markup changes may require a parser update. |

For resource ownership and concurrency guarantees, see [architecture](architecture.md). For reproduction checks, see [testing](testing.md).
