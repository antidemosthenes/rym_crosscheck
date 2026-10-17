# Build guide

This guide covers building the executable. For comparison commands, see the [README](../README.md); for test coverage and execution, see the [testing guide](testing.md).

## Requirements

- Windows x64 and MSVC with C++23 support, installed through Visual Studio or Visual Studio Build Tools.
- Windows SDK with the resource compiler, plus CMake 4+ and Ninja, available on PATH or through Visual Studio's bundled CMake tools.
- PowerShell and Visual Studio Installer's `vswhere` for the supplied helper.
- Internet access for the first dependency download. Existing verified archives are reused.
- An AVX2-capable CPU to run the optimized Release build.

Live comparisons also require a current Microsoft Edge version supporting hidden browser targets. The native reviewer requires the WebView2 Evergreen Runtime, detected at launch; the CLI and offline core tests can run without it. Building downloads the pinned WebView2 SDK and statically links its loader; no Node or frontend build tools are required.

## Configure, build, and test

Run these commands from the project folder. The helper discovers an installed C++ toolchain, initializes its x64 environment, and locates CMake and Ninja:

```powershell
./scripts/build.ps1 -Action all -Preset release
```

Use `-Preset debug` for a Debug build. Release enables optimization and supported link-time optimization; Debug provides debugging information and additional allocation checks in the tests.

| Helper option | Default | Behavior |
| --- | --- | --- |
| `-Preset NAME` | `release` | Choose `release` or `debug`, each with its own build directory. |
| `-Action configure` | — | Clear `build/` except `downloads/`, then generate build files; does not compile or run tests. |
| `-Action build` | — | Build enabled targets in an already configured directory; does not run tests. |
| `-Action test` | — | Run CTest against already built executables; does not build them. |
| `-Action all` | `all` | Clear `build/` except `downloads/`, configure, build, then run CTest. |
| `-BuildTests ON\|OFF` | `ON` | Explicitly set test compilation during `configure` or `all`. Use `OFF` with `configure`, then `build`; `all` runs CTest and rejects explicit `OFF`. |

The helper starts a fresh build for `configure` and `all`: it removes all contents of `build/` except `build/downloads/`, including other presets, old release archives, logs, and runtime data inside portable builds. Copy any report, whitelist, or settings you want to keep outside `build/` first, and close programs running from that directory. Cleanup failures stop the build. The separate `build` and `test` actions do not clean; they use the prepared configuration. The GitHub workflow uses `configure -BuildTests OFF` followed by `build`, retaining its restored download archives and test-disabled configuration.

For example, after configuring, build without running tests:

```powershell
./scripts/build.ps1 -Action build -Preset release
```

Alternatively, use an **x64 Visual Studio developer terminal** with CMake and Ninja on PATH:

```powershell
cmake --preset release
cmake --build --preset release --parallel 8
ctest --preset release
```

Replace `release` with `debug` in each command for Debug. Configuration prepares the build, building compiles enabled targets, and CTest executes registered tests. Each preset writes a portable installation under `build/<preset>/portable/`: `missing-report.exe` at its root and `bin/rym-crosscheck.exe` beneath it. Runtime files are created in its `data/` directory. Keep this layout together when copying a build.

## What does `--parallel 8` mean?

`--parallel 8` allows the build tool to schedule up to eight concurrent build jobs. It does not select eight tests or set the application's `--local-workers` or `--web-workers`. See the [CMake build command reference](https://cmake.org/cmake/help/latest/manual/cmake.1.html#build-a-project).

More parallel jobs can reduce build time when CPU and memory are available; fewer jobs reduce peak resource use. Use `--parallel 4` for fewer jobs or `--parallel 1` for a sequential build. Dependency sub-builds and compiler/linker threads can add processes, so this is not a limit on the total operating-system process count. The helper currently passes `--parallel 8`; use the manual commands to choose another value.

## Are tests built by default?

**Yes.** `RYM_BUILD_TESTS` defaults to `ON` on a fresh configuration. A normal Release or Debug build includes:

- `rym-crosscheck`: the comparison CLI.
- `missing-report`: the native reviewer, built even when tests are disabled.
- `rym-tests`: correctness and lifecycle checks.
- `rym-browser-visibility`: an optional live browser UI regression check.

Building these executables does not run them. CTest runs the registered correctness and CLI-help checks; the browser-visibility executable is invoked separately. The helper's `all` action runs CTest after building, while `build` does not. Upstream dependency tests are disabled independently of the project's test option.

## Build without tests

From the developer terminal, disable project test targets at configuration time, then build:

```powershell
cmake --preset release -DRYM_BUILD_TESTS=OFF
cmake --build --preset release --parallel 8
```

Skip CTest when tests are disabled. The equivalent helper commands are:

```powershell
./scripts/build.ps1 -Action configure -Preset release -BuildTests OFF
./scripts/build.ps1 -Action build -Preset release
```

`-Action build` alone does not disable test compilation. The GitHub [release workflow](releases.md) explicitly disables test builds, while ordinary fresh local configurations retain the default `ON`.

With direct CMake commands, the option persists in that configuration's CMake cache until you change it. Release and Debug use separate caches, so configure each independently. Disabling tests with direct CMake commands does not remove previously generated test executables or test metadata; their presence does not mean they were rebuilt. The helper's `configure` and `all` actions remove all previous configurations first, so pass `-BuildTests OFF` each time you start a fresh application-only configuration.

To restore test targets and run the checks:

```powershell
cmake --preset release -DRYM_BUILD_TESTS=ON
cmake --build --preset release --parallel 8
ctest --preset release
```

For a fresh application-only build directory, without existing test artifacts:

```powershell
cmake --preset release -B build/app-only -DRYM_BUILD_TESTS=OFF
cmake --build build/app-only --parallel 8
```

This writes the launcher to `build/app-only/portable/missing-report.exe` and the CLI to `build/app-only/portable/bin/rym-crosscheck.exe`.

For isolated validation that preserves an existing portable installation, use direct CMake commands with a separate binary directory as above. Do not invoke the helper's `configure` or `all` actions during that validation: they clear the entire shared `build/` tree, including custom binary directories.

## Dependencies and generated files

Dependencies are downloaded from pinned upstream archives, verified with SHA-256 and TLS verification, and built as static libraries with consistent MSVC runtime settings. No vcpkg installation is needed. Downloads remain under `build/downloads`; generated dependencies, binaries, and build logs stay under the Git-ignored `build` directory.

All nine dependencies share that archive cache across build configurations. Archive names contain the pinned SHA-256, so different versions can coexist. A matching existing archive is verified and reused without contacting its download server. Archives from the previous cache layout are also reused when their hashes match. Only missing pinned archives are downloaded. A corrupted archive in the current layout fails verification; remove the named file and configure again to replace it.

The helper's `build` action and direct incremental CMake builds reuse extracted sources and compiled dependencies. Direct CMake reconfiguration with unchanged dependency settings does not repeat extraction. The helper's `configure` and `all` actions clear those generated files, so dependencies must be extracted and compiled again; retained verified archives avoid network downloads. Only missing archives are downloaded. Keep `build/downloads` when manually removing configuration directories such as `build/release` or `build/debug`.

Dependency downloads are separate from the application's runtime cache. See [runtime storage and cleanup](runtime.md) for that distinction, [architecture](architecture.md) for implementation details, and [releases](releases.md) for packaging the Release executable.
