# RYM cross-check

Find songs in [RYM’s top all-time song chart](https://rateyourmusic.com/charts/top/song/all-time/) that are missing from your local MP3, FLAC, and M4A library. Review uncertain matches and manage exclusions in a small desktop reviewer.

## Quick start

Download and extract the Windows x64 `.7z` from [Releases](https://github.com/antidemosthenes/rym_crosscheck/releases). Keep this layout together:

```text
missing-report.exe
bin/
  rym-crosscheck.exe
data/
  missing.json
  whitelist.toml
  settings.toml
  cache/
```

The optimized programs require an AVX2-capable CPU and the [Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist). Live comparisons require Microsoft Edge. The reviewer additionally requires the [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/#download-section); it is a separate component from Edge. To compile from source, see the [build guide](docs/build.md).

Open **`missing-report.exe`**. It starts maximized at Overview. The left sidebar keeps navigation in the same place on every screen:

- **Start a new comparison:** choose your Music folder using Browse, enter Top N (default **200**), and click Start comparison. Advanced settings exposes the options in the tables below. The sidebar's **New comparison** opens the same form.
- **Results & whitelist:** review the saved comparison and exclusions. Either file can be absent; Load buttons remain available in the empty or error state. Summary counts distinguish missing songs, uncertain matches, and whitelist entries.

The Running screen separates **Chart songs**, such as `40 / 200`, from **Local tracks**, such as `640`. Local scanning runs alongside chart retrieval; its count appears as soon as scanning finishes. Unresolved files or directories are listed separately. It also shows CLI stages, elapsed time, and Cancel, without estimating a percentage. You cannot edit results or start another comparison during a run. Closing offers **Stop and close**, **Keep running**, or **Cancel**; stopping waits for cleanup, while Keep running leaves the viewer open.

Successful comparisons open Results automatically, including comparisons with incomplete-scan warnings. Failure or cancellation offers **Retry** and **View previous results**. Successful GUI comparisons remember reusable options in `data/settings.toml`; **Offline** and **Clear cache** reset for every new comparison. No terminal window opens for GUI comparisons.

The CLI also works independently. From the extracted folder, substitute your music directory:

```powershell
./bin/rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC'
```

Source-build users can open `./build/release/portable/missing-report.exe`. For a CLI comparison of a different chart size:

```powershell
./bin/rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --top 500
```

Both programs resolve the installation layout above independently of the terminal’s working directory. Explicit relative music, cache, and browser-profile CLI arguments resolve from the working directory. Your music directory is required; there is no default music path. Keep the installation in a writable directory.

## What happens during a run

1. **Scan your library.** Read MP3, FLAC, and M4A tags recursively, skipping directory links. Missing artist/title tags fall back to an `Artist - Title` filename. Unchanged files can reuse cached tags.
2. **Retrieve a fresh chart.** Collect complete ranks 1 through N. Chart and song retrieval stays invisible and can overlap local scanning.
3. **Resolve access if requested.** A dedicated extension-free Edge window opens for Cloudflare or RYM security checks, or when RYM redirects a chart page to sign-in. RYM’s restriction form requires completing reCAPTCHA **and clicking Submit**; comments and email are optional. For sign-in, use your RYM account in Edge and choose Remember me if offered and you want the session retained. After a security check, wait until the chart appears; after sign-in, the signed-in homepage is also fine. Then **close that Edge window to continue**. Standalone terminal users can also press Enter. Credentials stay in Edge; the program reuses its persistent session and resumes invisible collection. It does not solve security checks or sign in automatically.
4. **Compare and enrich.** Identify exact ownership and fuzzy suggestions. Whitelisted songs are excluded from current results. Album metadata is filled where possible; unavailable or ambiguous albums appear as `Unknown`.
5. **Save and review.** Atomically replace `data/missing.json`, then show Results. Failed comparisons preserve the previous report. Standalone interactive CLI runs open the viewer with `--show-results` and exit; `--no-open` suppresses this, and ordinary noninteractive CLI runs do not open it.

Reviewing saved results needs no RYM connection and does not rescan your library. Reopening the viewer reuses its existing window. A newly published report is announced without resetting your displayed whitelist or selections; **Load latest results** first checks for whitelist differences.

A fresh browser profile does not guarantee a visible security check: RYM or Cloudflare may allow access without asking for interaction. Sign-in is requested only when RYM redirects retrieval there. See [fresh-access testing and troubleshooting](docs/runtime.md#testing-fresh-access) for using a separate empty profile without changing your retained session.

## Review songs and customize the whitelist

The reviewer has two sections:

- **Current results:** all currently missing or uncertain songs not covered by the whitelist. Check **Add** for songs you want future comparisons to skip.
- **Whitelist:** collapsed initially, showing all exclusions, including entries outside the current chart. Expand it and check **Remove** to stop excluding an entry.

Each section has a search box and **Select all matching / Deselect all matching** buttons. Current results also filter by Missing or Needs review. Bulk selection covers every matching page, not just the 100 displayed rows. Changing filters keeps your choices; counts show selections outside the filter.

Click **Save changes** to apply additions and removals together. Until then, checkboxes do not change the files. Saving refreshes the current report and both sections. Removed exclusions return to current results when they belong to this chart, are not exactly owned, and are not covered by another entry. Entries outside the chart become eligible in later comparisons.

Whitelisting an uncertain match means **skip this RYM song**, not a confirmed association with a local file. The exclusion remains even if that local file is later removed.

### Saving and external edits

The displayed whitelist, with your staged selections applied, is the proposed complete whitelist. **Save changes replaces the local TOML with exactly that set**. External entries absent from the displayed set can be removed. Saving is available even with no staged selections.

Before leaving Results or replacing a file, the program compares canonical exclusion entries with disk. Any difference, including external edits made without staged selections, offers **Replace local / Discard / Cancel**. Replace local writes the proposed set; Discard preserves disk and abandons the displayed editing state; Cancel stays in Results. Formatting, comments, and entry order alone do not trigger a difference. External whitelist changes are not adopted continuously.

### Load existing data

Use **Load report JSON** or **Load whitelist** in Results. A native picker selects a file; validation runs in the background, then a preview shows its filename and song or exclusion count. Choose Replace or Cancel. Outstanding whitelist differences are resolved before replacement.

JSON loading replaces only `data/missing.json`; an embedded whitelist snapshot never replaces active TOML. TOML loading replaces only `data/whitelist.toml`, then refreshes the report snapshot and both sections. Sources are copied after validation and remain untouched. Selecting the active file is a no-op. Invalid input, cancellation, or failed destination writes preserve existing data. Imports accept program-generated report JSON and the whitelist syntax below; they do not create offline chart caches.

If the active whitelist is unreadable, exclusion checkboxes, bulk-selection controls, and Save are disabled. Results remain readable and you can leave the screen or load a valid whitelist; the unreadable file is not treated as an empty set to save.

### Existing and manually edited whitelists

There is **no automatic migration**. Existing `report/` files and whitelists beside older executables remain untouched. Load them explicitly through Results. A comparison initializes an empty `data/whitelist.toml` when the active file is absent.

You can still edit the active TOML yourself. Supported forms are:

```toml
[[songs]]
url = "https://rateyourmusic.com/song/radiohead/paranoid-android/"

[[songs]]
title = "Example song"
artist = "Example artist"

[[songs]]
title = "Example collaboration"
artists = ["Principal artist", "Guest artist"]
```

URL entries contain only `url`, an HTTPS RYM song-page URL. Artist/title entries require `title` and either `artist` or `artists`. List all credited artists; their order does not matter. Commas and `&` inside a name do not split it into multiple artists. Unknown keys, malformed TOML, or invalid entries produce an error.

The reviewer adds songs by URL and preserves artist/title entries in the displayed set. Saving normalizes TOML and deduplicates equivalent entries; **manual comments and formatting are not retained**. An unreadable whitelist must be repaired or explicitly replaced; ordinary Save does not overwrite it. Saving refreshes the latest report generation. If report refresh fails after the whitelist commit, a partial-success warning explains that the exclusions were saved; they are not rolled back.

Keep personal data out of Git. The generated `data/` directory and legacy personal file locations are ignored; `whitelist.example.toml` documents the supported syntax.

## Understanding results

Exact ownership requires matching title, complete credited artists, and a compatible recording version. Both RYM and local titles are normalized before exact or fuzzy comparison. Normalization ignores case, Unicode representation, extra whitespace, typographic quotes/dashes, explicit remaster suffixes, paired quotation marks surrounding a title, and spaces around `/`. A trailing included-section annotation such as `(Including 'Mirrors')` is optional. Original titles remain unchanged in the report.

| RYM title | Local title | Result with matching artist credits |
| --- | --- | --- |
| `"Heroes"` | `Heroes` | Owned |
| `21st Century Schizoid Man (Including 'Mirrors')` | `21st Century Schizoid Man` | Owned |
| `Weird Fishes / Arpeggi` | `Weird Fishes/ Arpeggi` | Owned |
| `Enjoy the Silence` | `Enjoy The Silence [Single Version]` | Needs review: Version differs |

Accents, internal apostrophes, meaningful punctuation, and ordinary section labels such as `(Part II)` remain significant. `Title (2020 Remaster)` can match `Title`, but `Title (Live)` remains distinct; the word in `Live Forever` is not a recording label. Structured artist credit order can differ.

Fuzzy suggestions require **90% artist and 85% title similarity by default**, plus compatible versions. **Credit mismatch** suggestions can appear when the principal artist matches exactly and the title passes its threshold, while full credits differ. **Version differs** permits review between unqualified, explicitly labeled single-version, and album-version titles: scores use the base title, but differing labels never establish automatic ownership. Live, remix, demo, and other protected recording qualifiers remain incompatible with an unqualified recording. Scores measure text similarity, not ownership probability.

**Albums have zero matching weight.** A song on a compilation or a different album can count as owned; album names are displayed for context only. An explicit version label in the title still matters regardless of the album.

Run another comparison to apply updated matching rules to a saved report. Offline comparisons also use the new rules with their saved chart and the current library.

| Display | Meaning |
| --- | --- |
| Missing | No local suggestion qualifies. |
| Needs review | Up to three distinct local suggestions, with scores and paths relative to your music directory. Duplicate editions share a suggestion slot. |
| Unknown album | Associated album metadata was unavailable or ambiguous; reporting still completes. |
| Local scan incomplete | Review the prominent warning and affected-file list. Reported songs may exist among unresolved files. |
| Offline snapshot | Source, capture time, and snapshot age are shown. |

The JSON stores comparison data so removing an exclusion can restore its row without another scan. Owned songs remain out of current results. The whitelist view uses known chart metadata or falls back to its stored URL or artist/title.

## CLI arguments

Run `./bin/rym-crosscheck.exe --help` for a summary. Flags take no value. `--output`, `--whitelist`, `--web-mode`, `--private-browser`, and `--persistent-browser` are not supported. Report and whitelist locations are fixed; Markdown output is no longer generated. Advanced GUI fields use the same defaults and ranges.

### Comparison and display

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--music-dir PATH` | Required | Readable music directory, scanned recursively. |
| `--top N` | `200` | Integer chart size `1`–`2147483647`. |
| `--offline` | Off | Use a complete snapshot from an earlier live run; still scan the current library. |
| `--no-open` | Off | Save the report without automatically opening the reviewer. |
| `-h`, `--help` | Off | Print help and exit without requiring a music path. |

### Retrieval and workers

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--browser-profile PATH` | `%LOCALAPPDATA%/rym-crosscheck/edge-profile` | Dedicated persistent Edge profile for verification and sign-in cookies; do not use a personal profile. |
| `--verification-timeout SECONDS` | `300` | Integer `1`–`3600`, in seconds; bounds security verification and manual sign-in. |
| `--local-workers N` | Hardware concurrency, capped at `8` | Concurrent tag readers; explicit integer `1`–`64`. |
| `--web-workers N` | `4` | Album lookup workers; integer `1`–`8`. Chart pagination is sequential. |
| `--request-delay-ms N` | `1000` | Nonnegative minimum interval between page-navigation starts, in milliseconds, shared across workers. |

Eight workers is an application cap, not a verified RYM server limit. Browser asset requests are separate from page-navigation pacing. See [runtime troubleshooting](docs/runtime.md#troubleshooting) for access restrictions.

### Matching and cache

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--artist-threshold VALUE` | `0.90` | Minimum ordinary artist similarity; decimal `0`–`1`, up to six fractional digits. |
| `--title-threshold VALUE` | `0.85` | Minimum title similarity; decimal `0`–`1`, up to six fractional digits. |
| `--cache-dir PATH` | Installation’s `data/cache` | Local tag records and program-generated chart snapshots. |
| `--clear-cache` | Off | Clear application cache records before comparison. Does not clear verification cookies. With `--offline`, it removes the required snapshot. |

| Exit code | Meaning |
| --- | --- |
| `0` | Comparison completed, including missing/review results. |
| `1` | Fatal error or cancellation. |
| `2` | Report saved with an incomplete local scan. |

A reviewer launch failure does not change a completed comparison’s exit code. Only one comparison can run per installation, including direct CLI runs. Preserve `data/` when upgrading and close both programs before replacing executables. See the release guide for safe updates.

## Further documentation

- [Build guide](docs/build.md): prerequisites, Release/Debug, parallel builds, and disabling tests.
- [Release guide](docs/releases.md): GitHub releases, packaging, and portable upgrades.
- [Runtime guide](docs/runtime.md): snapshots, caches, profiles, cleanup, and troubleshooting.
- [Testing guide](docs/testing.md): core correctness tests and native UI checks.
- [Architecture](docs/architecture.md): matching, report storage, concurrency, and ownership.
