# Runtime storage and troubleshooting

[README](../README.md) explains comparisons, access prompts, matching, and whitelist editing.

## Offline comparisons

A successful live comparison saves a chart snapshot.

```powershell
./bin/rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --offline --top 200
```

Offline requires the same `--cache-dir` as the live comparison.
The snapshot must have a compatible schema and complete ranks through Top N. A larger snapshot can serve a smaller Top N.

Offline makes no web requests. It scans the current library and applies current matching rules and exclusions.
Results show the snapshot source, capture time, and age.

Offline requires an explicit option. A failed live comparison does not silently use an old snapshot.
Report JSON imports do not create snapshots. The program does not accept manual HTML or CSV imports.

## Application cache

The default location is installation-relative `data/cache`.

| File | Contents |
| --- | --- |
| `tracks.json` | Raw tags, library identity, file paths, sizes, and modification times. |
| `chart.json` | The latest chart snapshot. |
| `run.lock` | The lock for cache access. |

Unchanged files reuse tags. Each comparison rebuilds identities.
An invalid tag cache causes another scan. An invalid snapshot prevents Offline.

Cleanup removes records older than 30 days. It then removes the oldest records until their total size is at most 128 MiB.
A snapshot can expire or disappear during this cleanup.

`--clear-cache` removes tag and chart records before comparison. A removal failure stops the comparison.
With Offline, this option removes the required snapshot.
It does not clear cookies, reports, exclusions, or dependency downloads.

Cache roots must be normal directories. Symbolic links and junctions are invalid roots.
Cache startup removes stale temporary writes from that cache directory.

## Browser profiles and cookies

The default profile is `%LOCALAPPDATA%/rym-crosscheck/edge-profile`.
`--browser-profile` selects another dedicated folder. Browser profiles are separate from application caches.

Do not select personal browser data. The program rejects roots named `User Data`, symbolic links, and junctions.

Retrieval, verification, and sign-in share one persistent Edge profile.
Edge keeps cookies and credentials. Extensions remain disabled.
Visible Edge windows serve access recovery. Normal retrieval stays invisible.

### Access recovery

Each affected chart page permits one security recovery and one sign-in recovery.
A retry keeps ranks from earlier pages.

| State | Program behavior |
| --- | --- |
| Recognized security page | Open ordinary Edge at the affected chart URL. |
| Sign-in redirect | Open the affected URL for manual sign-in. |
| Unusable page after 10 seconds | Offer browser access recovery. Redirects or changed markup can also cause this state. |
| Navigation error, exhausted retry, timeout, or cancellation | Stop. Keep the previous report. |
| Restricted album lookup | Stop queued album requests. Keep available albums and give a warning. |

The [README access instructions](../README.md#during-a-comparison) apply.
RYM's form requires reCAPTCHA and **Submit**. After sign-in, the homepage is sufficient.
Close the dedicated window to continue. Terminal users can also press Enter.

`--verification-timeout` limits each visible access period, not the whole comparison.
Security and sign-in have separate allowances. Additional time does not guarantee access.
Saved cookies do not prevent session expiry.

The 10-second limit applies to hidden page loading. Browser startup and shutdown can add time.
Recognized security checks and sign-in redirects start recovery without this wait.
Logs show the requested URL and the last observed URL when page loading times out.

Recovery opens the affected URL in a new Edge window with the saved profile.
It does not intentionally open the homepage or initialize cookies.
RYM can redirect to the homepage. A homepage does not supply the requested chart data.
Close that window to retry hidden retrieval. Repeated failure stops the comparison and preserves the previous report.

A redirected CLI cannot perform interactive verification by itself.
The GUI uses a checked launcher channel for its hidden CLI.
The program does not solve CAPTCHAs or sign in automatically.

### Testing fresh access

Choose a new, empty folder for a dedicated profile.

```powershell
./bin/rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --browser-profile 'PATH/TO/NEW/EMPTY/PROFILE' --no-open
```

Reuse this path to test session persistence.
Choose another new path to test access without the previous profile's cookies or storage.
Retrieval writes profile data even without a visible window.

An empty profile does not force a challenge. The service selects its checks.
Record only the gates that appear.
[Cloudflare's guide](https://developers.cloudflare.com/cloudflare-challenges/challenge-types/challenge-pages/) describes its challenge pages.

To reset a dedicated profile:

1. Close the comparison.
2. Close its dedicated Edge window.
3. Remove only that dedicated profile folder.

The next live comparison creates a new profile. `--clear-cache` does not reset Edge state.

## Workers, reports, and cleanup

Album lookup defaults to four workers and permits up to eight.
Workers share navigation pacing. The default interval is 1,000 ms between starts.
Additional workers overlap page loads without removing the delay.

These are application limits, not known server quotas. Browser asset requests are separate.
A restriction stops queued album requests. Other album failures leave `Unknown` values and warnings.
These failures do not prevent a report.

A successful comparison atomically replaces the report.
Incomplete chart ranks or failed access keep the previous report.
Unresolved local files produce an affected-file list and exit code `2`.

Controlled exits join workers, release handles, and remove temporary writes. They close owned browser processes.
Browser HTTP-cache clearing is best effort. Verification cookies and sign-in cookies remain.
Cleanup preserves music and personal browser data.

Forced termination can leave temporary files. Cleanup does not search arbitrary folders.
The [build helper](build.md#configure-build-and-test) has separate removal rules.

Tag caches contain local paths. Check generated data before sharing it.
Add ignore rules for custom cache or profile paths inside the repository.

## Reviewer state

The active files are `data/missing.json`, `data/whitelist.toml`, and `data/settings.toml`.
These paths use the installation directory, not the working directory.

Home does not parse the report. Results loads the report when necessary.
Reviewing makes no RYM requests.
[Whitelist instructions](../README.md#edit-the-whitelist) explain Save, external edits, and imports.

Each report has a generation identifier. Save refreshes the latest report generation.
An older reviewer cannot replace newer comparison data.
The whitelist commits first. A failed report refresh reports partial success without reversing that commit.

One comparison runs per installation. Cache and profile locks also apply across installations.
Independent installations require separate cache and profile paths.

The reviewer supervises its hidden CLI through an owned Job Object.
Cancellation requests cleanup. Stop and close waits for cleanup.
Unexpected reviewer termination prevents orphaned comparison children.

Report publication completes the comparison.
Later settings, cache, or reviewer start failures give warnings.

WebView2 uses a separate directory under `%LOCALAPPDATA%/rym-crosscheck/reviewer/` for each installation.
This directory does not share Edge verification cookies.
Close the reviewer before removing its runtime profile. Moving an installation can leave the old runtime profile.

A WebView2 startup failure offers the official runtime download page.
The reviewer does not install the runtime automatically. Saved files remain intact.

## Troubleshooting

| Problem | Action |
| --- | --- |
| Repeated verification | Complete the check. Select Submit if present. Wait for usable content. Close Edge. Reuse the same profile. |
| Restricted album lookup | Retry later. Reduce `--web-workers` to `1`. Increase `--request-delay-ms` to `2000`. |
| Edge cannot start | Check Edge installation, profile permissions, locks, and local browser-debugging policy. |
| Cache or profile lock | Finish the other comparison. Alternatively, use separate dedicated paths. |
| Lock error with a Windows number | Check permissions and directory availability. This error differs from a sharing lock. |
| Missing or small offline snapshot | Complete a live comparison with sufficient Top N and the same cache directory. |
| Unknown album | The program found no unique supported album. Album metadata does not determine ownership. |
| Incomplete local scan | Check the affected-file list, permissions, tags, and filename format. |
| Malformed whitelist | Correct the TOML or use Load whitelist to check a replacement. |
| Unsupported chart markup | Retry later. The parser can require an update. |

The program discards Edge's internal diagnostics. Application warnings remain visible.
Check the completion message and exit code.

Preserve `data/` during [upgrades](releases.md#portable-upgrades).
[Testing](testing.md) gives the manual checks.
