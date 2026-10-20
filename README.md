# RYM cross-check

Compare local music with [RYM's top all-time songs](https://rateyourmusic.com/charts/top/song/all-time/). Review possible local matches. Add unwanted songs to the whitelist to exclude them from later results.

## Quick start

1. Download the Windows x64 archive from [Releases](https://github.com/antidemosthenes/rym_crosscheck/releases).
2. Extract the archive to a writable folder.
3. Open **missing-report.exe**.
4. Select **Start a new comparison**.
5. Choose the Music folder.
6. Set Top N. The default is 200.
7. Select **Start comparison**.

The reviewer starts maximized. A second start uses the existing window.

Keep this folder structure:

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

The program creates data files when necessary. It requires Windows x64, an AVX2 CPU, and the [Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).
Live comparisons require Edge. The reviewer requires the [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/#download-section).

The [build guide](docs/build.md) gives source build instructions. The Release reviewer is `build/release/portable/missing-report.exe`.

## During a comparison

The command-line program (CLI) scans MP3, FLAC, and M4A files while it retrieves a fresh chart. It reads tags first.
If artist or title tags are absent, it tries the `Artist - Title` filename format. It skips directory links.
Unchanged files reuse cached tags.

Chart and song retrieval stays invisible. A dedicated Edge window opens for access recovery if hidden chart retrieval fails.

| Page | Required action |
| --- | --- |
| Cloudflare check | Complete the check. Wait for the chart. |
| RYM restriction form | Complete reCAPTCHA. Select **Submit**. Wait for the chart. |
| RYM sign-in | Sign in manually. The signed-in homepage is sufficient. |
| Normal homepage without an access prompt | Close the window. The program retries the requested chart with the saved session. |

**Close that Edge window to continue.** Terminal users can also press Enter.
Edge keeps credentials and cookies in its persistent profile. The program does not solve CAPTCHAs or enter credentials.
The [runtime guide](docs/runtime.md#access-recovery) explains the 10-second hidden-page wait and recovery limits.

The Running screen shows the stage, counts, elapsed time, and Cancel. Chart counts and local file counts are separate.
For example, `40 / 200` chart songs does not describe the library size.

Closing during a comparison gives these choices:

| Action | Result |
| --- | --- |
| Stop and close | The reviewer requests cancellation. It closes after cleanup. |
| Keep running | The reviewer stays open while the comparison continues. |
| Cancel | The Running screen remains open. |

A successful comparison saves `data/missing.json` and opens Results. A failed comparison keeps the previous report.
Failure offers **Retry** and **View previous results**. Successful reviewer comparisons save settings.
**Offline** and **Clear cache** reset for each new comparison.

Saved results require no network or scan. When a newer report is available, select **Load latest results**.
The reviewer checks pending whitelist differences before it replaces the screen contents.

## Results and matching

**Current results** excludes songs with exact ownership or a whitelist match.
**Whitelist** contains all exclusions, including entries outside the chart. This section starts collapsed.

### Matching decisions

The program uses three types of matching:

1. Exact ownership requires equal normalized identities.
2. Ordinary fuzzy matching requires at least **90% artist** and **85% title** similarity.
3. Review exceptions admit structural differences, Latin diacritics, or common title notation.

Normalization converts equivalent formatting to one form. Both fields normalize Unicode, case, whitespace, and typographic punctuation.
Titles also normalize outer quotes, slash spacing, and explicit remaster suffixes.

Exact ownership requires the title, complete artist credits, recording version, and included sections to match.
Artist order can differ in a structured list. The program does not split names at commas or `&`.
The principal artist is the first artist in the recorded list.
**Albums have zero matching weight.** A compilation can contain an owned song.

Review comparisons keep the original metadata:

| Field | Review rule |
| --- | --- |
| Title annotations | Remove balanced `(...)` and `[...]` blocks from a copy. Compare the remaining complete bases or components. |
| Title components | Split at `/` or a spaced dash (` - `). Require a complete-title match to one component. Internal substrings do not qualify. |
| Title quotes | Remove outer quote marks. Keep their words and internal apostrophes. |
| Numbered labels | Expand `Pt` to `Part`, `Vol` to `Volume`, and `No` to `Number` before digits. Accept optional periods and spacing. |
| Title credits | Remove trailing `feat`, `ft`, `feature`, or `featuring` text from a review copy. Accept an optional period. |
| Artist overlap | Require an exact base title and a contained phrase with at least two meaningful words. |
| Artist slash | Permit `/` as a review boundary. Keep the complete artist identity. `AC/DC` does not imply separate artists. |
| Latin diacritics | Compare a separate spelling, such as `ô` → `o`. Require equal complete titles or equal complete credits. |

Annotation removal accepts unknown labels. Empty bases and malformed pairs do not qualify.
These review rules never prove ownership. They can admit suggestions below the ordinary thresholds.

Title review uses this order:

1. Retain recording versions, included sections, artist credits, and original metadata.
2. Fold supported Latin diacritics in a copy.
3. Expand supported numbered labels outside guest names.
4. Remove trailing featured-artist text from that copy.
5. Keep the whole title. Create a base by removing balanced annotations.
6. Normalize resulting formatting before slash or spaced-dash segmentation.

The new notation rule requires equal complete forms or an existing complete-component relation.
It does not expand arbitrary acronyms, Roman numerals, or written numbers. Digit sequences retain leading zeros.
Title-credit removal requires a nonempty title and guest credit. It also accepts a final balanced credit block.
Artist tags remain unchanged. Existing recording checks still apply.

Recording conflicts normally block suggestions. Equal complete bases after annotation removal can expose recording conflicts for review.
A component match alone cannot override a recording conflict. Latin spelling preserves non-Latin text and exact whitelist identities.

### Labels and examples

| Label | Meaning |
| --- | --- |
| Missing | No local suggestion qualifies. |
| Needs review | Up to three local suggestions need your decision. |
| Credit mismatch | The recorded principal artist matches, but complete credits differ. |
| Artist name overlap | A contained artist phrase suggests a credit variant. |
| Diacritics differ | Latin accent removal makes complete titles or complete credits equal. |
| Version differs | Recording or edition labels differ, or one title lacks a label. |
| Included sections differ | Included-section names differ, or one title lacks the list. |
| Compound title differs | One complete title equals a slash or spaced-dash component of the other title. |
| Annotations differ | Annotation removal supplies a complete-base or component match. |
| Title notation differs | Numbered-label expansion or title-credit removal is necessary for this suggestion. |
| Unknown album | Album metadata is unavailable or ambiguous. |
| Local scan incomplete | The scan cannot resolve some local files or directories. |

| RYM metadata | Local metadata | Result |
| --- | --- | --- |
| `"Heroes"` | `Heroes`, same artist | Owned |
| `Weird Fishes / Arpeggi` | `Weird Fishes/ Arpeggi`, same artist | Owned |
| `War Pigs` | `War Pigs/Luke's Wall`, same artist | Needs review: Compound title differs |
| `Disorder` | `Disorder - 2019 Digital Master`, same artist | Needs review: Compound title differs |
| `War Pigs` | `War Pigs [Bonus]/Luke's Wall`, same artist | Needs review: Annotations differ, Compound title differs |
| `Title (Including 'A' and 'B')` | `Title`, same artist | Needs review: Included sections differ |
| `Enjoy the Silence` | `Enjoy the Silence [Single Version]`, same artist | Needs review: Version differs |
| `Move On Up` | `Move on Up (Extended Version)`, Curtis Mayfield | Needs review: Version differs |
| `Song` | `Song [Unknown label]`, same artist | Needs review: Annotations differ |
| `Song` | `Song (Live)`, same artist | Needs review: Version differs, Annotations differ |
| `All Along the Watchtower`, The Jimi Hendrix Experience | Same title, Jimi Hendrix | Needs review: Artist name overlap |
| `I'm God`, Clams Casino | Same title, Clams Casino/ Imogen Heap | Needs review: Artist name overlap |
| `Duvet`, Bôa | Same title, BoA | Needs review: Diacritics differ |
| `Luv (Sic.) Part 3`, Nujabes | `Luv (Sic.) Pt.3 feat. Shing02`, Nujabes | Needs review: Title notation differs |

The King Crimson included-section examples require review when local section names are absent.
Equivalent section lists can match despite differences in quotes or separators. Supported separators are `and`, `&`, and commas.

Scores measure text similarity, not ownership probability. Original normalized text supplies diacritic and notation scores.
**Segmented base title** scores use the text after annotation removal. Equal bases score 100%. Component scores can be lower.

Duplicate editions share one suggestion slot. Suggestions contain relative local paths.

An incomplete scan includes a warning and an affected-file list. Run a new comparison after matching rules change.
Offline comparisons also use current rules. Old reports keep their saved decisions.

## Edit the whitelist

1. Select **Add** for unwanted songs in Current results.
2. Expand Whitelist.
3. Select **Remove** for exclusions to remove.
4. Select **Save changes**.

Each section has separate search and bulk selection controls. Current results also has Missing and Needs review filters.
Each page contains 100 rows. **Select all matching** includes matching rows on every page.
Filters keep selections, including hidden selections. Selections remain pending until Save.

Removing an exclusion can restore a result row. The song must occur in the saved chart, lack exact ownership, and have no remaining exclusion.
An exclusion skips the RYM song. It does not establish ownership of a suggested local recording.

### Save and external edits

**Save replaces the entire disk whitelist with the shown entries and pending choices.** External entries absent from the screen can disappear.
The reviewer does not continuously adopt external edits.

Before navigation or file replacement, differences give these choices:

| Action | Result |
| --- | --- |
| Replace local | Write the complete proposed whitelist. |
| Discard | Keep disk entries. Abandon the screen's pending state. |
| Cancel | Keep the current screen and selections. |

Comments, formatting, and entry order do not count as differences. Save can apply external differences without checkbox selections.
Save removes duplicate entries and TOML comments. A failed report refresh does not reverse a successful whitelist save.
The reviewer reports that partial success.

### Load existing files

1. Select **Load report JSON** or **Load whitelist**.
2. Choose a source file.
3. Check the filename and count in the preview.
4. Select **Replace** or **Cancel**.

| Source | Replacement |
| --- | --- |
| Report JSON | Replace only `data/missing.json`. Keep the active TOML whitelist. |
| Whitelist TOML | Replace only `data/whitelist.toml`. Refresh the report and both sections. |

The reviewer keeps source files unchanged. Selecting the active destination changes nothing.
Invalid files and failed writes preserve existing data. Outstanding whitelist differences require a decision first.

No files migrate automatically. Old files require explicit loading. The program does not accept HTML, CSV, or Markdown reports.
Report imports do not create offline snapshots. An unreadable whitelist disables exclusion editing and Save.
Viewing, navigation, and Load remain available.

### Manual TOML entries

The active file is `data/whitelist.toml`. The CLI creates an empty file if this file is absent.

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

A URL entry contains only `url`. Other entries require `title` and either `artist` or `artists`.
Complete credits are necessary. Version, section, and accent differences remain significant for artist/title exclusions.

1. Copy [whitelist.example.toml](whitelist.example.toml) to a separate file.
2. Edit the copy.
3. Load the copy through the reviewer.

Unknown keys, invalid entries, and malformed TOML cause errors. Git ignores active personal data.

## CLI arguments

From the extracted folder:

```powershell
./bin/rym-crosscheck.exe --music-dir 'PATH/TO/MUSIC' --top 200
```

The CLI requires the music path. Explicit relative paths use the working directory.
Fixed data paths use the installation directory. Advanced reviewer fields use the same defaults and ranges.

### Comparison and output

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--music-dir PATH` | Required | Readable music folder. The scan includes subfolders. |
| `--top N` | `200` | Chart size from `1` through `2147483647`. |
| `--offline` | Off | Use a complete cached chart with the current local scan. |
| `--no-open` | Off | Do not start the reviewer after a standalone CLI comparison. |
| `-h`, `--help` | Off | Show help without a music path. |
| `--artist-threshold VALUE` | `0.90` | Ordinary artist similarity from `0` through `1`. Accept up to six decimal places. |
| `--title-threshold VALUE` | `0.85` | Ordinary title similarity from `0` through `1`. Accept up to six decimal places. |

### Workers and access

| Argument | Default | Meaning and accepted values |
| --- | --- | --- |
| `--local-workers N` | Hardware concurrency, capped at `8` | Tag readers from `1` through `64`. |
| `--web-workers N` | `4` | Album workers from `1` through `8`. Chart pages remain sequential. |
| `--request-delay-ms N` | `1000` | Global delay between navigation starts. Accept a nonnegative integer in milliseconds. |
| `--verification-timeout SECONDS` | `300` | Each visible access period. Accept `1` through `3600` seconds. |
| `--browser-profile PATH` | `%LOCALAPPDATA%/rym-crosscheck/edge-profile` | Persistent Edge profile. Use a dedicated application folder. |

Eight web workers is an application limit, not a known RYM limit. Browser asset requests are separate from navigation pacing.

### Cache

| Argument | Default | Meaning |
| --- | --- | --- |
| `--cache-dir PATH` | Installation's `data/cache` | Store tags and chart snapshots. |
| `--clear-cache` | Off | Remove application cache records before comparison. With Offline, this removes the required snapshot. |

Only one comparison can run per installation. Interactive CLI comparisons open Results after completion.
Noninteractive comparisons do not start the reviewer. A reviewer start failure gives a warning without changing the comparison's exit code.

| Exit code | Meaning |
| --- | --- |
| `0` | Completed, including Missing and Needs review rows. |
| `1` | Fatal error or cancellation. |
| `2` | Report saved with an incomplete local scan. |

## Documentation

- [Build](docs/build.md): requirements, build jobs, helper cleanup, and tests.
- [Runtime](docs/runtime.md): snapshots, caches, profiles, retries, and troubleshooting.
- [Testing](docs/testing.md): automated checks and optional manual checks.
- [Architecture](docs/architecture.md): matching algorithms, ownership, concurrency, and storage.
- [Releases](docs/releases.md): packages, publishing, notices, and upgrades.
