# Windows x64 releases

## Automatic publishing

The [workflow](https://github.com/antidemosthenes/rym_crosscheck/blob/main/.github/workflows/release.yml) runs on pushes to `main` and manual dispatch on `main`.
One push produces one release from its final commit.

The workflow builds the Release preset on Windows with Visual Studio 2026.
It disables project tests. It caches verified dependency downloads, not compiled objects or personal data.

Fresh runners still extract and compile dependencies. Helper cleanup keeps `build/downloads/`.
Only missing archives require downloads. [Build](build.md) explains the archive cache.

`GITHUB_TOKEN` requires `contents: write` for tags, assets, and releases.
Repository policies must permit these operations.

## Versions and retry behavior

| Item | Behavior |
| --- | --- |
| Initial version | Start at `v1.0.0`. |
| Later version | Increase the patch number of the highest stable tag. |
| Release title | Use the version tag. |
| Release notes | Use the full message of the triggering commit. |
| Existing commit tag | Reuse a stable tag already attached to that commit. |
| Published release | Keep the release intact. |
| Failed draft | Retry the draft. |

Workflow runs execute sequentially, with up to 100 pending runs.
Versions follow processing order.
[GitHub's guide](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency) explains concurrency controls.

The workflow packages and checks the archive before tag creation.
It creates a draft, uploads assets, and publishes the release.
A failure can leave a tag or draft for retry.
The workflow never moves an existing conflicting tag.

## Download contents

Assets use this naming pattern:

```text
rym-crosscheck-v1.0.0-windows-x64.7z
rym-crosscheck-v1.0.0-windows-x64.7z.sha256
```

The archive contains:

```text
rym-crosscheck-v1.0.0-windows-x64/
  missing-report.exe
  bin/rym-crosscheck.exe
  data/                         (empty)
  README.md
  docs/
  whitelist.example.toml
  LICENSE
  third-party-licenses/
  version.txt
```

Packaging copies an explicit file list.
It excludes tests, Debug binaries, reports, active whitelists, settings, caches, browser profiles, and music.

`version.txt` records the tag and commit. [README](../README.md#quick-start) lists runtime requirements.

Check the archive hash against its checksum file:

```powershell
Get-FileHash ./rym-crosscheck-v1.0.0-windows-x64.7z -Algorithm SHA256
```

## Manual packaging and publication

1. Close executables from the build directories.
2. Copy required personal data outside `build/`.
3. Run the commands below.

These commands remove old outputs except downloads:

```powershell
./scripts/build.ps1 -Action configure -Preset release -BuildTests OFF
./scripts/build.ps1 -Action build -Preset release
./scripts/release.ps1
```

Local packaging requires 7-Zip. It does not require GitHub CLI or authentication.
Outputs are under `build/releases/`: the archive, SHA-256 file, and `release-notes.txt`.
Packaging removes its temporary staging directory.

Publication requires a clean checkout.
Local binaries and documentation can include working-tree changes, but version metadata still identifies `HEAD`.
The workflow uses a fresh checkout.

For manual web publication:

1. Create a draft release for the built commit.
2. Use the version tag as the title.
3. Use `release-notes.txt` as the release notes.
4. Attach the archive and checksum assets.
5. Check the draft.
6. Publish the release.

Authenticated CLI publication uses:

```powershell
./scripts/release.ps1 -Publish -Repository OWNER/REPO
```

Packaging without `-Publish` creates no tag or GitHub release.

## Third-party sources and notices

[Dependencies.cmake](https://github.com/antidemosthenes/rym_crosscheck/blob/main/cmake/Dependencies.cmake) defines archive URLs and SHA-256 hashes.
The source tag identifies the exact dependency build.

| Dependency | Version | Source |
| --- | --- | --- |
| Boost | 1.92.0 | [Modular release](https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz) |
| TagLib | 2.3.2 | [Archive](https://github.com/taglib/taglib/archive/refs/tags/v2.3.2.tar.gz) |
| utf8cpp | 4.2.1 | [Archive](https://github.com/nemtrif/utfcpp/archive/refs/tags/v4.2.1.tar.gz) |
| zlib | 1.3.2 | [Archive](https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz) |
| utf8proc | 2.12.0 | [Archive](https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.12.0.tar.gz) |
| Lexbor | 3.0.0 | [Archive](https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.tar.gz) |
| CLI11 | 2.7.2 | [Archive](https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz) |
| toml++ | 3.4.0 | [Archive](https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz) |
| WebView2 SDK | 1.0.4258.31 | [Package](https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4258.31/microsoft.web.webview2.1.0.4258.31.nupkg) |

Release archives retain upstream licenses and notices.
TagLib uses its MPL option. Packaging retains both upstream license texts.
The project license is [LICENSE](../LICENSE).

## Portable upgrades

1. Close the reviewer.
2. Close the CLI.
3. Replace root `missing-report.exe`.
4. Replace `bin/rym-crosscheck.exe`.
5. Preserve `data/`, including the report, whitelist, settings, and cache.

A new installation can load old report JSON and whitelist files through Results.
Source files remain unchanged. Settings and caches are separate from imports.

Files do not migrate automatically from the old `report/` structure or root whitelist.
The reviewer does not import Markdown reports.
Removed arguments are `--output`, `--whitelist`, `--web-mode`, `--private-browser`, and `--persistent-browser`.
