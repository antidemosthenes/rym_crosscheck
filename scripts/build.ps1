param(
  [ValidateSet('configure','build','test','all')][string]$Action = 'all',
  [ValidateSet('release','debug')][string]$Preset = 'release',
  [ValidateSet('ON','OFF')][string]$BuildTests
)
$ErrorActionPreference = 'Stop'
if ($BuildTests -and $Action -notin @('configure','all')) { throw '-BuildTests applies to configure or all.' }
if ($BuildTests -eq 'OFF' -and $Action -eq 'all') { throw 'Use configure -BuildTests OFF, then build; all also runs CTest.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer with vswhere is required.' }
$buildTools = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -or -not $buildTools) { throw 'No Visual Studio installation with C++ build tools was found.' }
$vcvars = Join-Path $buildTools 'VC\Auxiliary\Build\vcvars64.bat'
$developerEnvironment = & cmd.exe /d /c ('"' + $vcvars + '" >nul && set')
if ($LASTEXITCODE) { throw 'Could not initialize the x64 Visual Studio environment.' }
$developerEnvironment | ForEach-Object {
  $parts = $_.Split('=', 2)
  if ($parts.Length -eq 2 -and $parts[0] -match '^[A-Za-z_][A-Za-z0-9_()]*$') { [Environment]::SetEnvironmentVariable($parts[0], $parts[1], 'Process') }
}
function Find-BuildTool([string]$Name, [string]$BundledPath) {
  $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($command) { return $command.Source }
  $bundled = Join-Path $buildTools $BundledPath
  if (Test-Path -LiteralPath $bundled) { return $bundled }
  throw "Required build tool is missing: $Name"
}
$cmake = Find-BuildTool 'cmake.exe' 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) { throw 'ctest must be installed alongside CMake.' }
$ninja = Find-BuildTool 'ninja.exe' 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
  if ($Action -in @('configure','all')) {
    $configureArguments = @('--preset', $Preset, "-DCMAKE_MAKE_PROGRAM=$ninja")
    if ($BuildTests) { $configureArguments += "-DRYM_BUILD_TESTS=$BuildTests" }
    & $cmake @configureArguments
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
  }
  if ($Action -in @('build','all')) { & $cmake --build --preset $Preset --parallel 8; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
  if ($Action -in @('test','all')) { & $ctest --preset $Preset; if ($LASTEXITCODE) { exit $LASTEXITCODE } }
} finally { Pop-Location }
