# Windows x64 releases

## Automatic publishing

The [release workflow](https://github.com/antidemosthenes/rym_crosscheck/blob/main/.github/workflows/release.yml) runs on pushes to `main`. Each push publishes the executable built from that push's final commit; pushing several commits together produces one release. You can also select **Actions → Windows x64 release → Run workflow** on `main` to retry a failed run.

The workflow uses a Windows runner with Visual Studio 2026, CMake, Ninja, GitHub CLI, and 7-Zip. It configures the **Release preset** with `RYM_BUILD_TESTS=OFF`, builds the CLI, and packages `build/release/bin/rym-crosscheck.exe`. It neither compiles nor runs project tests. Ordinary local build defaults are unchanged; see the [build guide](build.md) and [testing guide](testing.md).

Pinned, hash-verified dependency archives are cached between runs. Compiled objects, application caches, and browser profiles are not cached. The first run takes longer because it downloads and compiles all dependencies; subsequent runs reuse downloads but still compile from source.

The workflow's `GITHUB_TOKEN` requests `contents: write` to create tags, upload assets, and publish releases. No personal access token is needed. Repository or organization policies and tag rules must permit these operations. If they deny publication, the run fails with the GitHub error; local packaging remains available.

## Versions, titles, and notes

- The first release is **`v1.0.0`**. Subsequent releases increment the patch number of the highest stable `vMAJOR.MINOR.PATCH` tag: `v1.0.1`, `v1.0.2`, and so on. Prerelease and unrelated tags are ignored. Creating a higher stable major/minor tag manually starts that version line.
- The release title is its version tag. Its notes contain the triggering commit's full message, including the subject and body. Commit messages are read as data from Git and passed in a notes file.
- A retry of a tagged commit reuses its existing stable tag. An already published release is left intact. A failed draft can be completed on retry; published assets and notes are never overwritten.
- Release runs are serialized, with up to 100 pending runs queued rather than cancelled when another push arrives. Versions are assigned in processing order; see [GitHub's concurrency documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency).

No tag is created until compilation and archive verification succeed. Publishing first creates a draft, uploads both assets, and then makes it public. An upload failure can leave a tag and draft for the next retry.

## Download contents

Each release has two assets:

```text
rym-crosscheck-v1.0.0-windows-x64.7z
rym-crosscheck-v1.0.0-windows-x64.7z.sha256
```

The archive contains one folder with the optimized Release executable, README, documentation, `whitelist.example.toml`, project license, third-party license notices, and `version.txt` recording the tag and commit. It includes no test executables, debug build, personal whitelist, reports, music, browser data, or application cache.

The executable requires Windows x64, an AVX2-capable CPU, and the [Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist). Microsoft Edge is needed for live comparisons. Extract the archive before following the [README](../README.md).

To verify a downloaded archive, compare this output with the accompanying `.sha256` file:

```powershell
Get-FileHash ./rym-crosscheck-v1.0.0-windows-x64.7z -Algorithm SHA256
```

## Manual packaging and publication

From the repository root, build without tests and package locally:

```powershell
./scripts/build.ps1 -Action configure -Preset release -BuildTests OFF
./scripts/build.ps1 -Action build -Preset release
./scripts/release.ps1
```

Install 7-Zip or put `7z.exe` on PATH. Local packaging does not require GitHub CLI, credentials, or a network connection once dependencies are built. It creates the `.7z`, checksum, and `release-notes.txt` under Git-ignored `build/releases/`. Staging files are removed even on controlled failure. Only the Release preset's executable is packaged, regardless of whether Debug or test binaries also exist.

Before packaging manually, update your tags and build the intended source revision. Local packaging includes working-tree changes in the executable and documents; `version.txt` and notes identify `HEAD`. Use a clean checkout for a reproducible published release. The workflow always uses a fresh checkout of the triggering commit.

To publish manually, open the repository's **Releases → Draft a new release**, select or create the version tag for the built commit, use that version as the title, paste `release-notes.txt`, and upload the `.7z` and `.sha256` assets. Review and publish. Packaging by itself creates no Git tag or GitHub release.

For authenticated command-line publication with GitHub CLI, the same helper supports:

```powershell
./scripts/release.ps1 -Publish -Repository OWNER/REPO
```

Authenticate `gh` first. The workflow supplies `GITHUB_REPOSITORY` and its token automatically. A conflicting tag pointing to another commit is an error; the helper never moves an existing tag.

## Third-party sources and notices

Release archives preserve upstream license and notice files in `third-party-licenses/`. The following unmodified dependency sources correspond to this build; their pinned archive URLs and SHA-256 hashes are recorded in [Dependencies.cmake](https://github.com/antidemosthenes/rym_crosscheck/blob/main/cmake/Dependencies.cmake). To inspect the exact build, select the release tag in the source repository.

| Dependency | Version | Source archive |
| --- | --- | --- |
| Boost | 1.92.0 | [Modular release](https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz) |
| TagLib | 2.3.2 | [Source](https://github.com/taglib/taglib/archive/refs/tags/v2.3.2.tar.gz) |
| utf8cpp | 4.2.1 | [Source](https://github.com/nemtrif/utfcpp/archive/refs/tags/v4.2.1.tar.gz) |
| zlib | 1.3.2 | [Source](https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz) |
| utf8proc | 2.12.0 | [Source](https://github.com/JuliaStrings/utf8proc/archive/refs/tags/v2.12.0.tar.gz) |
| Lexbor | 3.0.0 | [Source](https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.tar.gz) |
| CLI11 | 2.7.2 | [Source](https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz) |
| toml++ | 3.4.0 | [Source](https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz) |

TagLib is distributed under its MPL option; its source is available through the link above. Both upstream license texts are retained for reference. The application license is in [LICENSE](../LICENSE).
