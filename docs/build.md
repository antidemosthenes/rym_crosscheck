# Build guide

This guide covers building the executable. For comparison commands, see the [README](../README.md); for test coverage and execution, see the [testing guide](testing.md).

## Requirements

- Windows x64 and MSVC with C++23 support, installed through Visual Studio or Visual Studio Build Tools.
- CMake 4+ and Ninja, available on PATH or through Visual Studio's bundled CMake tools.
- PowerShell and Visual Studio Installer's `vswhere` for the supplied helper.
- Internet access for the first dependency download. Existing verified archives are reused.
- An AVX2-capable CPU to run the optimized Release build.

Live comparisons also require a current Microsoft Edge version supporting hidden browser targets. Edge is not needed for ordinary offline correctness tests.

## Configure, build, and test

Run these commands from the project folder. The helper discovers an installed C++ toolchain, initializes its x64 environment, and locates CMake and Ninja:

```powershell
./scripts/build.ps1 -Action all -Preset release
```

Use `-Preset debug` for a Debug build. Release enables optimization and supported link-time optimization; Debug provides debugging information and additional allocation checks in the tests.

| Helper option | Default | Behavior |
| --- | --- | --- |
| `-Preset NAME` | `release` | Choose `release` or `debug`, each with its own build directory. |
| `-Action configure` | — | Generate build files; does not compile or run tests. |
| `-Action build` | — | Build enabled targets in an already configured directory; does not run tests. |
| `-Action test` | — | Run CTest against already built executables; does not build them. |
| `-Action all` | `all` | Configure, build, then run CTest. |
| `-BuildTests ON\|OFF` | Keep cached value; `ON` on a fresh build | Explicitly set test compilation during `configure` or `all`. Use `OFF` with `configure`, then `build`; `all` runs CTest and rejects explicit `OFF`. |

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

Replace `release` with `debug` in each command for Debug. Configuration prepares the build, building compiles enabled targets, and CTest executes registered tests. The application is written to `build/release/bin/rym-crosscheck.exe` or `build/debug/bin/rym-crosscheck.exe`.

## What does `--parallel 8` mean?

`--parallel 8` allows the build tool to schedule up to eight concurrent build jobs. It does not select eight tests or set the application's `--local-workers` or `--web-workers`. See the [CMake build command reference](https://cmake.org/cmake/help/latest/manual/cmake.1.html#build-a-project).

More parallel jobs can reduce build time when CPU and memory are available; fewer jobs reduce peak resource use. Use `--parallel 4` for fewer jobs or `--parallel 1` for a sequential build. Dependency sub-builds and compiler/linker threads can add processes, so this is not a limit on the total operating-system process count. The helper currently passes `--parallel 8`; use the manual commands to choose another value.

## Are tests built by default?

**Yes.** `RYM_BUILD_TESTS` defaults to `ON` on a fresh configuration. A normal Release or Debug build includes:

- `rym-crosscheck`: the application.
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

The option persists in that configuration's CMake cache until you change it. Release and Debug use separate caches, so configure each independently. Disabling tests does not remove previously generated test executables or test metadata from an existing build directory; their presence does not mean they were rebuilt.

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

This writes the application to `build/app-only/bin/rym-crosscheck.exe`.

## Dependencies and generated files

Dependencies are downloaded from pinned upstream archives, verified with SHA-256 and TLS verification, and built as static libraries with consistent MSVC runtime settings. No vcpkg installation is needed. Downloads remain under `build/downloads`; generated dependencies, binaries, and build logs stay under the Git-ignored `build` directory.

Dependency downloads are separate from the application's runtime cache. See [runtime storage and cleanup](runtime.md) for that distinction, [architecture](architecture.md) for implementation details, and [releases](releases.md) for packaging the Release executable.
