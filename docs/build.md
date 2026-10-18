# Build guide

[README](../README.md) explains program use. [Testing](testing.md) explains test execution.

## Requirements

- Windows x64.
- MSVC with C++23 support.
- The Windows SDK and resource compiler.
- CMake 4 or later, and Ninja.
- PowerShell and Visual Studio Installer's `vswhere` for the helper.
- Internet access for missing dependency archives.
- An AVX2 CPU for Release executables.

The helper finds the x64 Visual Studio tools and bundled CMake/Ninja.
Direct commands require an x64 developer terminal with these tools on PATH.

The helper initializes Visual Studio with a short temporary `PATH` to avoid CMD's expanded-line limit.
It adds the original search paths for build commands and removes duplicate paths.
It restores the terminal's environment after success or failure. Repeated calls do not accumulate developer environment settings.

Builds require no vcpkg, Node, frontend framework, or local HTTP server.
The SDK supplies a static WebView2 loader. [Runtime requirements](../README.md#quick-start) apply when the programs run.

## Configure, build, and test

**Helper actions `configure` and `all` remove everything under `build/` except `build/downloads/`.**
This removal includes other presets, archives, and portable runtime data.

1. Close executables from the build directories.
2. Copy required personal data outside `build/`.
3. Run the selected helper action from the repository root.

```powershell
./scripts/build.ps1 -Action all -Preset release
```

| Action | Behavior |
| --- | --- |
| `configure` | Remove outputs. Generate new build files. |
| `build` | Build enabled targets. Keep existing outputs. |
| `test` | Run CTest without a build. |
| `all` | Remove outputs. Configure and build. Run CTest. |

The default action is `all`. `-Preset` accepts `release` or `debug`. The default is `release`.
Release enables optimization and supported link-time optimization. Debug enables debug information and allocation checks in tests.

`-BuildTests ON|OFF` applies only to `configure` or `all`.
The helper rejects explicit `OFF` with `all`, because `all` runs CTest.

These commands reuse an existing configuration:

```powershell
./scripts/build.ps1 -Action build -Preset release
./scripts/build.ps1 -Action test -Preset release
```

Direct CMake commands preserve other build directories:

```powershell
cmake --preset release
cmake --build --preset release --parallel 8
ctest --preset release
```

Use `debug` in place of `release` for Debug. Each preset produces this structure:

```text
build/<preset>/portable/missing-report.exe
build/<preset>/portable/bin/rym-crosscheck.exe
```

Runtime files use the portable installation's `data/` folder.

## Build parallelism

`--parallel 8` permits up to eight concurrent build jobs. More jobs can shorten the build but increase CPU and memory use.
`--parallel 4` or `--parallel 1` reduces this load. The helper uses eight jobs.

Build jobs do not change application workers or select tests.
Dependency builds and compiler/linker threads can create additional processes.
The [CMake reference](https://cmake.org/cmake/help/latest/manual/cmake.1.html#build-a-project) describes this option.

## Build without tests

`RYM_BUILD_TESTS` defaults to `ON`.
Ordinary Release and Debug builds include both applications, `rym-tests`, and `rym-browser-visibility`.

A build does not run the tests. CTest runs correctness and CLI-help checks.
The live browser check runs separately.

From an x64 developer terminal:

```powershell
cmake --preset release -DRYM_BUILD_TESTS=OFF
cmake --build --preset release --parallel 8
```

The helper equivalent is:

```powershell
./scripts/build.ps1 -Action configure -Preset release -BuildTests OFF
./scripts/build.ps1 -Action build -Preset release
```

Both applications still build. If test targets are absent, skip CTest.
CMake disables upstream dependency tests independently.

Direct CMake settings persist in that build directory. `-DRYM_BUILD_TESTS=ON` restores test targets.
Disabling tests does not remove old test binaries or metadata.

Helper configuration removes prior settings. Each new application-only helper configuration requires `-BuildTests OFF`.

A separate application-only directory uses:

```powershell
cmake --preset release -B build/app-only -DRYM_BUILD_TESTS=OFF
cmake --build build/app-only --parallel 8
```

Do not use helper `configure` or `all` during isolated checks. These actions also remove custom directories under `build/`.

## Dependencies and downloads

Pinned archives use SHA-256 checks and TLS verification. Dependencies use static libraries and consistent MSVC runtime settings.
[Dependencies.cmake](https://github.com/antidemosthenes/rym_crosscheck/blob/main/cmake/Dependencies.cmake) defines versions and hashes.
[ArchiveCache.cmake](https://github.com/antidemosthenes/rym_crosscheck/blob/main/cmake/ArchiveCache.cmake) supplies the shared archive cache.

Verified archives remain in `build/downloads/`. Their names include hashes.
Existing valid archives require no server contact. Only missing archives require downloads.
The cache also checks legacy archive locations.

A corrupt current archive stops configuration. Remove the named archive before another configuration.

Incremental builds reuse extracted sources and compiled dependencies.
Direct reconfiguration preserves unchanged dependencies. Fresh helper configuration repeats extraction and compilation, but keeps downloads.

Git ignores build files and downloads. Dependency downloads are separate from the [application cache](runtime.md#application-cache).
