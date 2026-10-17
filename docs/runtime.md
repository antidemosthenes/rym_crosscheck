# Runtime storage and troubleshooting

See the [README](../README.md) for CLI arguments, verification instructions, whitelist configuration, matching, and report statuses. This guide explains persistent state and recovery.

## Offline comparisons

A successful live comparison saves a chart snapshot in the selected application cache. To compare against that snapshot while rescanning your current library:

```powershell
./build/release/portable/bin/rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --offline --top 200
```

The snapshot must contain complete ranks 1 through the requested N and use a compatible cache schema. A snapshot with more songs can serve a smaller N. Offline mode makes no web requests or album lookups, and the report shows the snapshot's source, capture time, and age. The local library and whitelist are evaluated for the current run.

If the live run used `--cache-dir`, supply the same directory offline. The GUI exposes Offline under Advanced and resets it for every new comparison. Offline is explicit; failed live access does not silently select an old chart. Loading a report JSON does not populate this cache. Manual HTML/CSV imports are not supported. An ordinary live run fetches a fresh chart even when a snapshot already exists.

## Application cache

By default, the installation’s `data/cache` directory contains:

| Record | Purpose |
| --- | --- |
| `tracks.json` | Previously read tags, library identity, file paths, sizes, and modification times. Unchanged files reuse tags; changed files are read again. |
| `chart.json` | The most recently saved chart snapshot for offline comparisons. |
| `run.lock` | Prevent concurrent use of the same cache directory. |

Cache records are versioned. Incompatible or unusable tag caches are ignored and tags are rescanned; an unusable chart snapshot prevents an offline comparison. Pruning removes application records older than 30 days and evicts the oldest records when their combined size exceeds 128 MiB. A snapshot can therefore become unavailable through age or size eviction.

`--clear-cache` removes tag and chart records before the requested comparison. Use it with a live run to rebuild the records. A deletion failure stops the comparison rather than silently reusing records. Combining it with `--offline` removes the snapshot that offline mode needs. It does not clear browser verification cookies, reports, whitelist files, music, or dependency downloads. Cache roots must be normal directories, not symbolic links or junctions.

## Browser profiles and cookies

The default dedicated profile is `%LOCALAPPDATA%/rym-crosscheck/edge-profile`. This environment-variable location is independent of the application cache. Override it with `--browser-profile PATH` when necessary, using an application-specific directory. The program rejects a profile directory named `User Data` regardless of case, and rejects symbolic-link or junction profile roots; avoid pointing it at personal browser data.

All live retrieval uses the persistent dedicated Edge profile. Verification and retrieval share its cookies, which stay inside Edge and are retained so later runs may not need another Cloudflare verification. Extensions are disabled for both retrieval and verification. Chart and song collection is invisible; the visible window is reserved for security verification and manual RYM sign-in as described in the [README](../README.md#what-happens-during-a-run).

Chart acquisition requests verification for recognized security pages and for pages that remain unusable after the 30-second loading deadline. The visible window opens the affected chart URL, including later pagination pages. Closing it retries that page and keeps ranks already collected. If the same access problem persists after its retry, the run stops with a clear error and preserves the previous report; it does not open an endless sequence of verification windows. A loading timeout can also indicate changed markup, so opening verification does not guarantee recovery. Explicit browser navigation errors are reported directly.

RYM also has its own restriction form, separate from Cloudflare. Complete its reCAPTCHA and click **Submit**, leaving optional comments/email empty if desired. Wait for the chart before closing the window; checking the box alone may not submit the form. A redirect to `/account/login` requests manual RYM sign-in separately from security verification. The program opens ordinary Edge at the affected chart URL, waits for you to finish and close it (the signed-in homepage is acceptable), then retries that page while keeping collected ranks. Credentials are entered only in Edge, and the session stays in that persistent profile. Choose Remember me if offered to retain sign-in after closing Edge. Each affected page permits at most one security-check recovery and one sign-in recovery; an unsuccessful retry preserves the previous report. `--verification-timeout` also bounds the manual sign-in wait.

The application locks the selected browser profile while using it. A separate installation-wide comparison lock rejects a second GUI or CLI comparison before data writes or cache clearing. Separate installations also need separate cache and browser-profile paths to run independently.

| Retrieval state | Behavior |
| --- | --- |
| Recognized security page | Pause collection and open ordinary Edge at the affected chart URL. Complete the check, wait for the chart, and close that window. |
| RYM sign-in redirect | Sign in manually in the dedicated window. The signed-in homepage is acceptable; close the window and the CLI retries the chart URL itself. |
| Chart remains unusable for 30 seconds | Offer the security-check handoff. This can also indicate changed markup; the retry is bounded. |
| Navigation error, unsuccessful retry, timeout, or cancellation | Stop chart collection and preserve the previous report. |
| Album lookup cannot complete | Stop queued enrichment requests and complete the comparison with available albums and warnings. Missing album information does not determine ownership. |

The configured access timeout applies to each visible handoff, not to the whole comparison. Security verification and sign-in have separate retry allowances. Sessions can expire or be invalidated by RYM even when retained locally. The program does not enter credentials or solve CAPTCHAs.

## Testing fresh access

Choose a new, empty application-specific Browser profile path under Advanced, or pass one explicitly:

```powershell
./bin/rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --browser-profile 'PATH/TO/NEW/EMPTY/PROFILE' --no-open
```

This separates the test from the default profile without deleting its saved session. Reuse the same test path to check session persistence; choose another new path to start without its cookies or site storage. A profile used by retrieval is no longer empty even if no visible window appeared. `--clear-cache` clears application records, not Edge state.

An empty profile does not force a particular challenge. Cloudflare can verify a browser without interaction, and the site controls whether verification or sign-in is required. See [Cloudflare's challenge types](https://developers.cloudflare.com/cloudflare-challenges/challenge-types/challenge-pages/). Record the checks actually shown rather than assuming that a normal page proves old cookies were reused. After testing, close the comparison and its Edge window before removing only that disposable profile.

## Reports and cleanup

Album lookups use four workers by default, with a maximum of eight. All workers share the default 1,000 ms interval between page-navigation starts; additional workers overlap slower page loads rather than creating an immediate burst. This is an application limit, not a server-approved quota. Browser asset requests are not counted as separate worker jobs, and RYM can still challenge a run within these settings.

If an album page encounters a security challenge, queued page navigations in that browser session are stopped. Already retrieved albums are retained, and unresolved albums remain `Unknown` with warnings. No automatic retry loop is used to probe the restriction.

Reports are written through a temporary sibling file and replaced atomically. Failed chart retrieval, incomplete chart ranks, or failed verification do not overwrite an existing report. Album lookup failures alone permit reporting with `Unknown` albums. Local scan warnings produce a report with an affected-file list and exit code `2`; see [report interpretation](../README.md#understanding-results).

On controlled exit, the program joins its workers, releases file/network handles, removes temporary writes, and closes only browser processes it launched. It attempts to clear browser HTTP cache during retrieval teardown, while persistent verification and sign-in cookies remain. Source/build files, music, whitelist files, reports, and personal browser data are preserved. This runtime cleanup is separate from the build helper's [destructive fresh-build cleanup](build.md#configure-build-and-test).

Forced termination can leave temporary files behind. Cache startup removes stale application temporary writes in its own cache directory. Other leftovers are not automatically swept from arbitrary locations.

The default report, cache, personal whitelist, editor settings, and build output are Git-ignored. Custom cache or browser-profile locations inside the repository need appropriate ignore rules. The fixed installation-relative report folder is ignored. Tag caches contain local file paths and metadata; review generated reports before sharing them. Dependency downloads under `build/downloads` are retained for subsequent builds and are not subject to runtime cache pruning.

## Troubleshooting

Browser standard input and output are isolated from the terminal. Edge's internal renderer, sync, and DevTools diagnostics are discarded; the CLI still reports startup failures, verification requests, retrieval errors, and album warnings. Older releases can print these browser diagnostics even during a successful comparison. A final comparison summary and report path indicate completion; check the exit code for an incomplete local scan.

| Symptom | Action |
| --- | --- |
| Cloudflare verification repeats or expires | Complete verification in the dedicated window and close that window. Standalone terminal users can also press Enter. Reuse the same profile to retain cookies. Increasing `--verification-timeout` gives more time; it does not guarantee access. |
| RYM's security check interrupts album lookup | Let the restriction clear or complete verification when requested on a later run. Reduce `--web-workers` and increase `--request-delay-ms`, for example to `1` worker and `2000` ms. More verification time does not raise a server limit. |
| Verification is required in a noninteractive run | Use the GUI launcher, an interactive terminal, or an available offline snapshot. A validated hidden GUI run can request verification; an arbitrary redirected CLI run cannot. Verification is not automated. |
| Edge cannot start or a browser command fails | Check that a current Edge version is installed, the dedicated profile is writable and unused, and local browser debugging is permitted. |
| `Album unavailable or ambiguous` | The song page did not identify one supported album unambiguously. Its album is `Unknown`; ownership matching and the report still complete. This message alone does not identify a parsing bug or a network failure. |
| The cache or profile is locked | Let the other run finish, or choose a separate cache and dedicated profile. |
| Cannot open a lock file, with a Windows error number | Check directory/file permissions and availability. This differs from another process holding the lock. |
| No cached chart, incompatible snapshot, or too few ranks | Complete a live run with at least the desired `--top` value using the same cache directory. |
| The local scan is incomplete | Review the affected-file list, permissions, tags, and filename fallback, then rerun. |
| Whitelist parsing fails | Use Load whitelist to validate a replacement, or check the [TOML examples and entry rules](../README.md#existing-and-manually-edited-whitelists). Existing data is not silently repaired. |
| Chart ranks are incomplete or markup is unsupported | Preserve the previous report and retry later; upstream markup changes may require a parser update. |

For resource ownership and concurrency guarantees, see [architecture](architecture.md). For reproduction checks, see [testing](testing.md).

## Reviewer files and lifecycle

The root viewer is `missing-report.exe`; the CLI is `bin/rym-crosscheck.exe`. Active files are `data/missing.json`, `data/whitelist.toml`, and `data/settings.toml`. These locations do not follow the working directory, and `--output`/`--whitelist` are not accepted. Standalone CLI auto-opening uses the viewer’s `--show-results` entry when input and output are interactive and `--no-open` is absent. A launch failure is a warning after the report is saved.

Reviewing is offline. Report parsing is deferred until Results is requested; Home can open even when the files are absent or malformed. The displayed whitelist is loaded on entry and remains authoritative while editing. It is not continuously refreshed from disk. Save writes exactly the displayed set with staged additions/removals. Departure, replacement, and newly published-report loading compare canonical entries and require Replace local, Discard, or Cancel for differences, including external edits without selections. Report refresh always uses the latest generation; a partial-success warning means the whitelist commit succeeded.

Load report JSON and Load whitelist use native pickers, background validation, and a filename/count confirmation. Sources remain untouched; only the chosen destination is replaced atomically. JSON’s embedded whitelist does not replace active TOML. Selecting the active destination does nothing. Imports never seed offline caches. There is no automatic migration from the old layout; use these actions to copy old program JSON or whitelist files explicitly.

Successful GUI comparisons save reusable options. Offline and Clear cache are omitted from remembered settings and reset on every new comparison. Settings-write failures are warnings after completion. The GUI supervises a hidden CLI child, drains bounded diagnostics, and requests cooperative cancellation; Stop and close waits for cleanup rather than detaching. Unexpected viewer termination closes its owned job, preventing orphaned comparison processes. Atomic report publication is the completion boundary.

WebView2 uses its own profile under `%LOCALAPPDATA%/rym-crosscheck/reviewer/`, separated per installation. It does not use the Edge verification profile or its cookies. The runtime’s own profile data is separate from the application’s bounded tag/chart cache. Closing the viewer closes its controller and releases COM resources; it does not close a personal browser or remove runtime files still in use. Moving an installation leaves its prior reviewer profile available until you remove it while no reviewer is running.

If the WebView2 runtime is missing or initialization fails, the viewer explains the failure and offers the official download page. It does not install anything automatically. The saved comparison and whitelist remain available. If the viewer executable is missing, restore the complete release layout; a fresh comparison is not required just to reopen a valid report.

For upgrades, close both programs, replace their executables, and preserve `data/`. See [portable upgrades](releases.md#portable-upgrades). Avoid putting personal data or settings in release archives. If the installation folder is read-only, copy it to a writable location before comparisons or saving.
