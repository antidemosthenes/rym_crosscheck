param(
  [ValidateSet('configure','build','test','all')][string]$Action = 'all',
  [ValidateSet('release','debug')][string]$Preset = 'release',
  [ValidateSet('ON','OFF')][string]$BuildTests
)
$ErrorActionPreference = 'Stop'
if ($BuildTests -and $Action -notin @('configure','all')) { throw '-BuildTests applies to configure or all.' }
if ($BuildTests -eq 'OFF' -and $Action -eq 'all') { throw 'Use configure -BuildTests OFF, then build; all also runs CTest.' }
function Clear-BuildOutputs([string]$ProjectRoot) {
  $projectPath = (Resolve-Path -LiteralPath $ProjectRoot).Path
  $buildPath = [IO.Path]::GetFullPath((Join-Path $projectPath 'build'))
  if (-not (Test-Path -LiteralPath $buildPath)) { return }
  $buildDirectory = Get-Item -LiteralPath $buildPath -Force
  if (-not $buildDirectory.PSIsContainer -or
      ($buildDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'Build cleanup requires a real build directory, not a file or directory link.'
  }
  Write-Host 'Removing build outputs; preserving build/downloads.'
  foreach ($item in Get-ChildItem -LiteralPath $buildPath -Force) {
    if ($item.PSIsContainer -and $item.Name -ieq 'downloads') { continue }
    $targetPath = [IO.Path]::GetFullPath($item.FullName)
    if (-not [string]::Equals([IO.Path]::GetDirectoryName($targetPath), $buildPath,
                              [StringComparison]::OrdinalIgnoreCase)) {
      throw "Refusing to remove a path outside the build directory: $targetPath"
    }
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
      Remove-Item -LiteralPath $targetPath -Force
    } else {
      Remove-Item -LiteralPath $targetPath -Recurse -Force
    }
  }
}
function Find-BuildTool([string]$Name, [string]$BundledPath) {
  $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($command) { return $command.Source }
  $bundled = Join-Path $buildTools $BundledPath
  if (Test-Path -LiteralPath $bundled) { return $bundled }
  throw "Required build tool is missing: $Name"
}
$originalEnvironment = [Environment]::GetEnvironmentVariables('Process')
try {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer with vswhere is required.' }
  $buildTools = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($LASTEXITCODE -or -not $buildTools) { throw 'No Visual Studio installation with C++ build tools was found.' }
  $vcvars = Join-Path $buildTools 'VC\Auxiliary\Build\vcvars64.bat'
  $cmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
  $compilerVariables = @('PATH', 'INCLUDE', 'LIB', 'LIBPATH', 'EXTERNAL_INCLUDE', 'IFCPATH',
    'VSINSTALLDIR', 'VCINSTALLDIR', 'DevEnvDir', 'VisualStudioVersion', 'WindowsLibPath',
    'UniversalCRTSdkDir', 'UCRTVersion', 'NETFXSDKDir')
  foreach ($name in $originalEnvironment.Keys) {
    if ($name -in $compilerVariables -or
        $name -match '^(__VSCMD|VSCMD|__VCVARS|VCTools|WindowsSDK|Framework)|^VS\d+COMNTOOLS$') {
      [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process')
    }
  }
  $env:Path = (Join-Path $env:SystemRoot 'System32') + ';' + $env:SystemRoot
  $developerEnvironment = & $cmd /d /c ('"' + $vcvars + '" >nul && set')
  if ($LASTEXITCODE) { throw 'Could not initialize the x64 Visual Studio environment.' }
  $developerEnvironment | ForEach-Object {
    $parts = $_.Split('=', 2)
    if ($parts.Length -eq 2 -and $parts[0] -match '^[A-Za-z_][A-Za-z0-9_()]*$') { [Environment]::SetEnvironmentVariable($parts[0], $parts[1], 'Process') }
  }
  $seenPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  $env:Path = (@($env:Path.Split(';')) + @(([string]$originalEnvironment['PATH']).Split(';')) |
    Where-Object { $_ -and $seenPaths.Add($_.TrimEnd('\')) }) -join ';'
  $cmake = Find-BuildTool 'cmake.exe' 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
  if (-not (Test-Path -LiteralPath $ctest)) { throw 'ctest must be installed alongside CMake.' }
  $ninja = Find-BuildTool 'ninja.exe' 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
  Push-Location (Split-Path -Parent $PSScriptRoot)
  try {
    if ($Action -in @('configure','all')) {
      Clear-BuildOutputs (Get-Location).Path
      $configureArguments = @('--preset', $Preset, "-DCMAKE_MAKE_PROGRAM=$ninja")
      if ($BuildTests) { $configureArguments += "-DRYM_BUILD_TESTS=$BuildTests" }
      & $cmake @configureArguments
      if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    if ($Action -in @('build','all')) { & $cmake --build --preset $Preset --parallel 8; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
    if ($Action -in @('test','all')) { & $ctest --preset $Preset; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
  } finally { Pop-Location }
} finally {
  foreach ($name in [Environment]::GetEnvironmentVariables('Process').Keys) {
    if (-not $originalEnvironment.Contains($name)) { [Environment]::SetEnvironmentVariable($name, [NullString]::Value, 'Process') }
  }
  foreach ($name in $originalEnvironment.Keys) {
    [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name], 'Process')
  }
}
