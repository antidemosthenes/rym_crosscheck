$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/release.ps1')

function Assert-Equal($Actual, $Expected) {
  if ($Actual -cne $Expected) { throw "Expected '$Expected', got '$Actual'." }
}

Assert-Equal (Get-ReleaseTag) 'v1.0.0'
Assert-Equal (Get-ReleaseTag @('unrelated', 'v1.0.0-rc.1', 'v01.0.0')) 'v1.0.0'
Assert-Equal (Get-ReleaseTag @('v1.0.9', 'v1.0.2', 'v1.0.10')) 'v1.0.11'
Assert-Equal (Get-ReleaseTag @('v1.9.9', 'v2.0.0', 'v1.10.0')) 'v2.0.1'
Assert-Equal (Get-ReleaseTag @('v1.0.8') @('v1.0.2')) 'v1.0.2'
Assert-Equal (Get-ReleaseTag @('v1.0.8') @('unrelated', 'v1.0.3', 'v1.0.2')) 'v1.0.3'

$script:commands = [Collections.Generic.List[object]]::new()
$script:tagExists = $false
$script:releaseState = 'absent'
$script:targetCommit = 'built-commit'
function Get-Command { return [pscustomobject]@{Name='gh'} }
function Invoke-ReleaseGh([string[]]$Arguments, [switch]$Json, [switch]$AllowFailure) {
  $script:commands.Add($Arguments)
  if ($Arguments[0] -eq 'api') {
    if ($Arguments -contains 'POST' -or $script:tagExists) {
      return [pscustomobject]@{object=[pscustomobject]@{type='commit'; sha=$script:targetCommit}}
    }
    return $null
  }
  if ($Arguments[1] -eq 'view') {
    if ($script:releaseState -eq 'absent') { return $null }
    return [pscustomobject]@{isDraft=($script:releaseState -eq 'draft'); url='https://example.com/release'}
  }
}
function Test-Publish { Publish-Release -Repository 'owner/repo' -Tag 'v1.0.0' -Commit 'built-commit' `
  -Archive 'archive.7z' -Checksum 'archive.7z.sha256' -Notes 'notes.txt' }
function Get-Calls { return ,@($script:commands | ForEach-Object { $_ -join ' ' }) }

Test-Publish
$calls = Get-Calls
Assert-Equal ($calls -match 'POST').Count 1
Assert-Equal ($calls -match 'release create .* --draft ').Count 1
Assert-Equal ($calls -match 'release upload').Count 1
Assert-Equal $calls[-1] 'release edit v1.0.0 --repo owner/repo --draft=false --latest'

$script:commands.Clear()
$script:tagExists = $true
$script:releaseState = 'draft'
Test-Publish
$calls = Get-Calls
Assert-Equal ($calls -match 'POST|release create').Count 0
Assert-Equal ($calls -match 'release upload').Count 1
Assert-Equal $calls[-1] 'release edit v1.0.0 --repo owner/repo --draft=false --latest'

$script:commands.Clear()
$script:releaseState = 'published'
Test-Publish
Assert-Equal ((Get-Calls) -match 'release (upload|edit|create)').Count 0

$script:commands.Clear()
$script:targetCommit = 'different-commit'
$rejected = $false
try { Test-Publish } catch {
  if ($_.Exception.Message -notmatch 'does not point to the built commit') { throw }
  $rejected = $true
}
Assert-Equal $rejected $true
Assert-Equal ((Get-Calls) -match 'release (upload|edit|create)').Count 0
Write-Host 'Release versioning and publishing lifecycle checks passed (no network requests).'
