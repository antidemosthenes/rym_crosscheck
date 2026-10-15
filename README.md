# RYM cross-check

Find songs in [RYM's top all-time song chart](https://rateyourmusic.com/charts/top/song/all-time/) that are missing from your local MP3, FLAC, and M4A library. Confirmed local matches and whitelist entries are excluded. Uncertain matches stay in a Markdown report for you to review.

## Quick start

Download the Windows x64 `.7z` archive from [Releases](https://github.com/antidemosthenes/rym_crosscheck/releases) and extract it. The optimized executable requires an AVX2-capable CPU and the [Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist). Live comparisons require Microsoft Edge and internet access. To compile it yourself, follow the [build guide](docs/build.md).

From the extracted folder, replace `PATH/TO/MUSIC` with your music directory. Source-build users can substitute `./build/release/bin/rym-crosscheck.exe` for `./rym-crosscheck.exe` in these commands:

```powershell
./rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC'
```

This compares the top **200** songs and writes **`missing.md`** in the working directory. Your music directory is always required; there is no default music path. Quote paths containing spaces. All relative paths resolve from the directory where you run the command.

To compare a different number of songs or choose another report:

```powershell
./rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --top 500 --output reports/missing.md
```

## What happens during a run

1. **Scan your library.** The program recursively reads MP3, FLAC, and M4A metadata, skipping directory links. Tags supply the title and artists; missing fields fall back to an `Artist - Title` filename. Unchanged files can reuse previously read tags.
2. **Retrieve the chart.** An ordinary live run retrieves fresh ranks 1 through N. Chart and song pages are collected invisibly; you do not need to navigate pages yourself. Local scanning and chart retrieval can overlap.
3. **Complete verification if requested.** If Cloudflare or RYM's security check challenges chart access, a dedicated, extension-free Edge window opens. Complete verification there, then press **Enter in the terminal** or close that browser window. The program reuses its cookies and resumes invisible retrieval. Verification is not solved automatically. A timeout or failed chart retrieval leaves the previous report intact.
4. **Cross-check songs.** Exact local matches and whitelist entries are excluded. Similar local songs become review suggestions rather than confirmed ownership.
5. **Fill in albums and write the report.** Missing album metadata is looked up where possible. Unavailable or ambiguous albums appear as `Unknown`. A successful comparison replaces the report atomically, and the terminal prints the comparison count, report location, and elapsed time.

The dedicated browser profile retains verification cookies between runs. The program always uses this persistent browser behavior for live retrieval. See [runtime behavior and troubleshooting](docs/runtime.md) for profile management and access failures.

## CLI arguments

Run `./rym-crosscheck.exe --help` for the command-line summary. A flag such as `--offline` takes no value. Defaults below apply when an argument is omitted.

### Comparison and output

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--music-dir PATH` | Required; none | Readable local music directory, scanned recursively. |
| `--top N` | `200` | Number of chart songs to compare; a positive integer. |
| `--output PATH` | `missing.md` | Markdown report path; must have a `.md` extension and cannot overwrite the whitelist. Parent directories are created if needed. |
| `--whitelist PATH` | `whitelist.toml` | TOML exclusions; see [customizing the whitelist](#customizing-the-whitelist). |
| `--offline` | Off | Use a complete chart snapshot from a previous live run instead of contacting RYM. Your current local library is still scanned. |
| `-h`, `--help` | Off | Print help and exit; no music path is needed for help. |

### Retrieval and verification

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--browser-profile PATH` | `%LOCALAPPDATA%/rym-crosscheck/edge-profile` | Persistent dedicated Edge profile location. Use an application-specific directory, not your personal browser profile. Verification cookies are retained here. |
| `--verification-timeout SECONDS` | `300` | Time allowed for visible verification; an integer from `1` to `3600` seconds. |

Browser and verification settings apply to live runs. Offline mode performs no web requests. See the [runtime guide](docs/runtime.md) for offline prerequisites and browser troubleshooting.

### Workers and fuzzy matching

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--local-workers N` | Automatic, `1`–`8` | Concurrent tag-reading workers. Automatic selection uses reported hardware concurrency, capped at eight; an explicit value may be `1`–`64`. |
| `--web-workers N` | `4` | Concurrent album lookup workers; `1`–`8`. Chart pagination remains sequential. |
| `--request-delay-ms N` | `1000` | Minimum interval between request starts across web workers; a nonnegative integer in milliseconds. More workers do not remove this pacing. |
| `--artist-threshold VALUE` | `0.90` | Minimum full-credit artist similarity for ordinary fuzzy suggestions. Decimal from `0` to `1`, with at most six fractional digits. |
| `--title-threshold VALUE` | `0.85` | Minimum title similarity for fuzzy suggestions. Decimal from `0` to `1`, with at most six fractional digits. |

Eight workers is an application cap, not a verified RYM server limit. Workers control page tasks; Edge can also request page assets. Shared pacing limits page-navigation starts, not every underlying HTTP request. For access failures or security checks, see [runtime troubleshooting](docs/runtime.md#troubleshooting).

### Application cache

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--cache-dir PATH` | `.rym-crosscheck-cache` | Store reusable tags and program-generated chart snapshots here. |
| `--clear-cache` | Off | Remove saved tag and chart records before comparison. Does not clear browser cookies. Combining it with `--offline` removes the snapshot needed for that run. |

Cache limits, retention, and cleanup are covered in the [runtime guide](docs/runtime.md).

## Customizing the whitelist

A whitelist suppresses songs you do not want reported, even when no exact local match exists. Start from the supplied example, then edit the copy:

```powershell
Copy-Item whitelist.example.toml whitelist.toml
```

Remove or replace its sample entries. Use one `[[songs]]` table per song, choosing one of these forms:

```toml
# Exclude one song by its RYM song-page URL.
[[songs]]
url = "https://rateyourmusic.com/song/radiohead/paranoid-android/"

# Exclude a title credited to one artist.
[[songs]]
title = "Example song"
artist = "Example artist"

# Exclude a title with multiple credited artists.
[[songs]]
title = "Example collaboration"
artists = ["Principal artist", "Guest artist"]
```

URL entries contain only `url`, pointing to an HTTPS RYM song page. Artist/title entries require `title` and either `artist` or `artists`, not both. List every credited artist separately when using `artists`; order does not matter. A name containing a comma or `&` remains one artist name. Artist/title entries use the same normalization and recording-version distinctions as exact matching.

To use another whitelist:

```powershell
./rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --whitelist 'PATH/TO/WHITELIST.toml'
```

An absent default `whitelist.toml` means no exclusions. An explicitly supplied missing file, invalid TOML, unknown keys, or invalid entry types produces an error. The default personal whitelist is Git-ignored; `whitelist.example.toml` is part of the project. A custom whitelist elsewhere needs its own ignore rule if stored in the repository.

## Understanding matches and the report

Exact ownership requires a matching title, complete credited artists, and a compatible recording version. Matching ignores case, Unicode representation, extra whitespace, typographic quotes/dashes, and explicit remaster suffixes. For example, `Title (2020 Remaster)` can match `Title`. Accents and meaningful punctuation remain significant. `Title (Live)` is distinct from `Title`; the ordinary word in `Live Forever` is not a recording label. Structured artist lists can match regardless of credit order.

Fuzzy matching compares normalized artist and title strings. Ordinary suggestions must meet both thresholds—**90% artist and 85% title by default**—and have compatible versions. Scores measure text similarity, not the probability that you own a song. Lower thresholds admit more suggestions; review them before changing your whitelist.

A separate **Credit mismatch** suggestion can appear when the principal artist matches exactly and the title meets its threshold, but the full artist lists differ. This can appear below the ordinary artist threshold. Album names never determine ownership.

The report lists songs in chart-rank order, with song titles linked to their RYM pages. This synthetic example illustrates the columns:

| Song | Artist | Album | Status | Possible local matches |
| --- | --- | --- | --- | --- |
| [Example song](https://rateyourmusic.com/song/example-artist/example-song/) | Example artist | Example album | Missing | |
| [Example variant](https://rateyourmusic.com/song/example-artist/example-variant/) | Example artist | Example album | Needs review | Example artist/Example varient.flac — artist 100.0%, title 93.3% |
| [Example collaboration](https://rateyourmusic.com/song/example-artist/example-collaboration/) | Example artist, Guest artist | Unknown | Needs review | Example artist/Example collaboration.mp3 — Credit mismatch; title 100.0% |

- **Missing:** no qualifying local suggestion was found.
- **Needs review:** up to three distinct local suggestions are shown with scores or a credit-mismatch reason and paths relative to the music directory. Duplicate editions occupy one suggestion slot.
- **Unknown album:** associated album metadata was unavailable or ambiguous; the comparison still completes.
- **Local scan incomplete:** a prominent warning and affected-file list indicate files or directories that could not be identified. Some reported songs might exist among those files.

Owned and whitelisted songs do not appear as rows. If nothing remains, the report says all compared songs are owned or whitelisted. Offline reports also show the snapshot source and age.

| Exit code | Meaning |
| --- | --- |
| `0` | Comparison completed, including any missing or review rows. |
| `1` | Fatal error or cancellation. |
| `2` | Report written with an incomplete local scan. |

## Further documentation

- [Build guide](docs/build.md): prerequisites, Release/Debug, parallel builds, and disabling test builds.
- [Release guide](docs/releases.md): automatic GitHub releases, versioning, and manual packaging.
- [Runtime guide](docs/runtime.md): offline snapshots, caches, browser profiles, cleanup, and troubleshooting.
- [Testing guide](docs/testing.md): CTest coverage and the optional live UI regression check.
- [Architecture](docs/architecture.md): matching algorithms, subsystem boundaries, and resource ownership.
