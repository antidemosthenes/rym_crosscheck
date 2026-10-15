param([switch]$Publish, [string]$Repository = $env:GITHUB_REPOSITORY)
$ErrorActionPreference = 'Stop'

function Get-ReleaseTag([string[]]$Tags = @(), [string[]]$CommitTags = @()) {
  $stablePattern = '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
  $current = @($CommitTags | Where-Object { $_ -cmatch $stablePattern } |
    Sort-Object { [version]$_.Substring(1) } -Descending)
  if ($current.Count) { return $current[0] }
  $stable = @($Tags | Where-Object { $_ -cmatch $stablePattern } |
    Sort-Object { [version]$_.Substring(1) } -Descending)
  if (-not $stable.Count) { return 'v1.0.0' }
  $last = [version]$stable[0].Substring(1)
  if ($last.Build -eq [int]::MaxValue) { throw 'Release patch version overflow.' }
  return 'v{0}.{1}.{2}' -f $last.Major, $last.Minor, ($last.Build + 1)
}

function Invoke-ReleaseGit([string[]]$Arguments) {
  $result = @(& git @Arguments)
  if ($LASTEXITCODE) { throw "git failed: $($Arguments[0])" }
  return $result
}

function Invoke-ReleaseGh([string[]]$Arguments, [switch]$Json, [switch]$AllowFailure) {
  $ErrorActionPreference = 'Continue'
  $result = @(& gh @Arguments 2>&1)
  $status = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  if ($status) {
    if ($AllowFailure) { return $null }
    throw "GitHub command failed: gh $($Arguments -join ' ')`n$($result -join "`n")"
  }
  if ($Json) { return ($result -join "`n" | ConvertFrom-Json) }
  if ($result.Count) { Write-Host ($result -join "`n") }
}

function Publish-Release([string]$Repository, [string]$Tag, [string]$Commit,
                        [string]$Archive, [string]$Checksum, [string]$Notes) {
  if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw 'Publishing requires -Repository OWNER/REPO or GITHUB_REPOSITORY.'
  }
  if (-not (Get-Command gh -CommandType Application -ErrorAction SilentlyContinue)) {
    throw 'GitHub CLI (gh) is required to publish; local packaging does not require it.'
  }
  $ref = Invoke-ReleaseGh -Arguments @('api', "repos/$Repository/git/ref/tags/$Tag") -Json -AllowFailure
  if (-not $ref) {
    $ref = Invoke-ReleaseGh -Arguments @('api', '--method', 'POST', "repos/$Repository/git/refs",
      '-f', "ref=refs/tags/$Tag", '-f', "sha=$Commit") -Json
  }
  $target = $ref.object
  for ($depth = 0; $target.type -eq 'tag' -and $depth -lt 8; ++$depth) {
    $annotation = Invoke-ReleaseGh -Arguments @('api', "repos/$Repository/git/tags/$($target.sha)") -Json
    $target = $annotation.object
  }
  if ($target.type -ne 'commit' -or $target.sha -ne $Commit) {
    throw "Refusing to publish: $Tag does not point to the built commit $Commit."
  }
  $release = Invoke-ReleaseGh -Arguments @('release', 'view', $Tag, '--repo', $Repository,
    '--json', 'isDraft,url') -Json -AllowFailure
  if ($release -and -not $release.isDraft) {
    Write-Host "Already published; keeping existing assets and notes: $($release.url)"
    return
  }
  if (-not $release) {
    Invoke-ReleaseGh -Arguments @('release', 'create', $Tag, '--repo', $Repository,
      '--verify-tag', '--draft', '--title', $Tag, '--notes-file', $Notes)
  } else {
    Invoke-ReleaseGh -Arguments @('release', 'edit', $Tag, '--repo', $Repository,
      '--title', $Tag, '--notes-file', $Notes)
  }
  Invoke-ReleaseGh -Arguments @('release', 'upload', $Tag, $Archive, $Checksum,
    '--repo', $Repository, '--clobber')
  Invoke-ReleaseGh -Arguments @('release', 'edit', $Tag, '--repo', $Repository,
    '--draft=false', '--latest')
}

function New-ReleasePackage {
  $root = Split-Path -Parent $PSScriptRoot
  Push-Location $root
  try {
    $commit = (Invoke-ReleaseGit @('rev-parse', '--verify', 'HEAD') | Select-Object -First 1).Trim()
    $tag = Get-ReleaseTag -Tags @(Invoke-ReleaseGit @('tag', '--list')) `
      -CommitTags @(Invoke-ReleaseGit @('tag', '--points-at', 'HEAD'))
    $binary = Join-Path $root 'build/release/bin/rym-crosscheck.exe'
    $cache = Join-Path $root 'build/release/CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $binary) -or -not (Test-Path -LiteralPath $cache)) {
      throw 'Build the release preset before packaging.'
    }
    if (-not (Select-String -LiteralPath $cache -Pattern '^CMAKE_BUILD_TYPE:STRING=Release$' -Quiet)) {
      throw 'The release build directory must contain a Release configuration.'
    }
    $sevenZip = Get-Command 7z.exe -CommandType Application -ErrorAction SilentlyContinue |
      Select-Object -First 1 -ExpandProperty Source
    if (-not $sevenZip) {
      $sevenZip = Join-Path $env:ProgramFiles '7-Zip/7z.exe'
      if (-not (Test-Path -LiteralPath $sevenZip)) { throw 'Install 7-Zip or put 7z.exe on PATH.' }
    }
    $output = [IO.Path]::GetFullPath((Join-Path $root 'build/releases'))
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $staging = Join-Path $output ('.staging-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    try {
      $name = "rym-crosscheck-$tag-windows-x64"
      $package = Join-Path $staging $name
      New-Item -ItemType Directory -Path $package | Out-Null
      Copy-Item -LiteralPath $binary -Destination $package
      foreach ($file in @('README.md', 'LICENSE', 'whitelist.example.toml', 'docs')) {
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination $package -Recurse
      }
      $utf8 = New-Object System.Text.UTF8Encoding($false)
      [IO.File]::WriteAllText((Join-Path $package 'version.txt'), "$tag`n$commit`n", $utf8)
      $sources = [ordered]@{
        boost = '_deps/boost-src'
        utf8proc = '_deps/utf8proc-src'
        lexbor = '_deps/lexbor-src'
        cli11 = '_deps/cli11-src'
        tomlplusplus = '_deps/tomlplusplus-src'
        taglib = 'taglib_project-prefix/src/taglib_project'
        utf8cpp = 'utf8cpp_project-prefix/src/utf8cpp_project'
        zlib = 'zlib_project-prefix/src/zlib_project'
      }
      foreach ($dependency in $sources.Keys) {
        $source = Join-Path $root (Join-Path 'build/release' $sources[$dependency])
        $licenses = @(Get-ChildItem -LiteralPath $source -File |
          Where-Object { $_.Name -match '^(LICENSE|COPYING|NOTICE)' })
        if (-not $licenses.Count) { throw "Missing dependency notices: $dependency" }
        $destination = Join-Path $package "third-party-licenses/$dependency"
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        $licenses | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $destination }
      }
      $temporaryArchive = Join-Path $staging "$name.7z"
      Push-Location $staging
      try {
        & $sevenZip a -t7z $temporaryArchive $name -mx=9 -mmt=on | Out-Host
        if ($LASTEXITCODE) { throw '7-Zip packaging failed.' }
        & $sevenZip t $temporaryArchive | Out-Host
        if ($LASTEXITCODE) { throw '7-Zip integrity verification failed.' }
      } finally { Pop-Location }
      $archive = Join-Path $output "$name.7z"
      Move-Item -LiteralPath $temporaryArchive -Destination $archive -Force
      $checksum = "$archive.sha256"
      $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
      [IO.File]::WriteAllText($checksum, "$hash  $name.7z`n", $utf8)
      $notes = Join-Path $output 'release-notes.txt'
      $message = (Invoke-ReleaseGit @('-c', 'i18n.logOutputEncoding=utf-8', 'show', '-s', '--format=%B', $commit)) -join "`n"
      [IO.File]::WriteAllText($notes, $message.TrimEnd() + "`n", $utf8)
      return [pscustomobject]@{ Tag=$tag; Commit=$commit; Archive=$archive; Checksum=$checksum; Notes=$notes }
    } finally {
      $resolvedStaging = [IO.Path]::GetFullPath($staging)
      if ((Split-Path -Parent $resolvedStaging) -ne $output -or
          (Split-Path -Leaf $resolvedStaging) -notmatch '^\.staging-[0-9a-f]{32}$') {
        throw 'Refusing cleanup outside the release staging directory.'
      }
      Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
    }
  } finally { Pop-Location }
}

if ($MyInvocation.InvocationName -ne '.') {
  $package = New-ReleasePackage
  Write-Host "Packaged $($package.Tag): $($package.Archive)"
  if ($Publish) {
    Publish-Release -Repository $Repository -Tag $package.Tag -Commit $package.Commit `
      -Archive $package.Archive -Checksum $package.Checksum -Notes $package.Notes
  }
}
