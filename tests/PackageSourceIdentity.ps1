# P1.30: a package can be traced to one commit, or it is not built.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File PackageSourceIdentity.ps1 -Tools <repo>\tools
#
# Runs tools/source_identity.ps1 against a scratch repository: a clean tree at
# the release tag packages, and names that commit; a dirty tree is refused,
# snapshot or not; an untagged HEAD, or a tag that moved away from HEAD, is
# refused unless the package is a snapshot.
param([Parameter(Mandatory)][string]$Tools)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $Tools 'source_identity.ps1')

$failures = New-Object Collections.Generic.List[string]
function Expect-Refusal {
    param([string]$Name, [scriptblock]$Action, [string]$Says)
    try { & $Action | Out-Null; $failures.Add("${Name}: packaged, but should have been refused") }
    catch { if ("$_" -notlike "*$Says*") { $failures.Add("${Name}: refused with '$_', expected '*$Says*'") } }
}
function Invoke-ScratchGit { $ErrorActionPreference = 'Continue'; & git -C $script:repo @args 2>&1 | Out-Null; if ($LASTEXITCODE) { throw "git $args failed" } }

$scratch = Join-Path ([IO.Path]::GetTempPath()) ("PackageSourceIdentity-" + [Guid]::NewGuid().ToString('N'))
$script:repo = $scratch
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    Invoke-ScratchGit init -q
    Invoke-ScratchGit config user.email test@example.invalid
    Invoke-ScratchGit config user.name test
    Invoke-ScratchGit config commit.gpgsign false
    Invoke-ScratchGit config tag.gpgsign false
    # What a Windows runner checks out with: Git for Windows' default.
    Invoke-ScratchGit config core.autocrlf true
    Set-Content -LiteralPath (Join-Path $scratch 'VERSION') -Value '1.2.3' -NoNewline
    $asset = Join-Path $scratch 'asset.txt'
    [IO.File]::WriteAllText($asset, "line one`nline two`n")
    Invoke-ScratchGit add VERSION asset.txt
    Invoke-ScratchGit commit -q -m one
    $tag = 'dlss5-video-player-v1.2.3'

    Expect-Refusal 'untagged release' { Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag } 'is not tagged'
    $snapshot = Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag -Snapshot
    $head = (& git -C $scratch rev-parse HEAD).Trim()
    if ($snapshot.Commit -cne $head -or $snapshot.Tag) { $failures.Add("snapshot: named '$($snapshot.Commit)'/'$($snapshot.Tag)', expected $head and no tag") }

    Invoke-ScratchGit tag $tag
    $release = Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag
    if ($release.Commit -cne $head -or $release.Tag -cne $tag) { $failures.Add("release: named '$($release.Commit)'/'$($release.Tag)', expected $head and $tag") }

    # A tracked change: refused whether or not the package is a release.
    Set-Content -LiteralPath (Join-Path $scratch 'VERSION') -Value '1.2.4' -NoNewline
    Expect-Refusal 'dirty release' { Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag } 'uncommitted changes'
    Expect-Refusal 'dirty snapshot' { Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag -Snapshot } 'uncommitted changes'
    Invoke-ScratchGit checkout -q -- VERSION
    # An untracked file is not part of what is packaged.
    Set-Content -LiteralPath (Join-Path $scratch 'notes.txt') -Value 'scratch'
    [void](Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag)

    # The same content with other line endings is not a change. A fetch
    # script rewrote a CRLF checkout with upstream's LF, git status called it
    # modified for good, and CI refused to package a clean tree.
    Remove-Item -LiteralPath $asset
    Invoke-ScratchGit checkout -q -- asset.txt
    [IO.File]::WriteAllText($asset, "line one`nline two`n")
    [void](Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag)
    # A deleted tracked file is a change.
    Remove-Item -LiteralPath $asset
    Expect-Refusal 'deleted file' { Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag } 'uncommitted changes'
    Invoke-ScratchGit checkout -q -- asset.txt

    # The tag stays behind while HEAD moves on: the moved-tag case, seen from
    # the packager.
    Set-Content -LiteralPath (Join-Path $scratch 'VERSION') -Value '1.2.3 ' -NoNewline
    Invoke-ScratchGit commit -q -am two
    Expect-Refusal 'tag behind HEAD' { Get-PackageSourceIdentity -RepositoryRoot $scratch -ExpectedTag $tag } 'not HEAD'

    Expect-Refusal 'no repository' { Get-PackageSourceIdentity -RepositoryRoot ([IO.Path]::GetTempPath()) -ExpectedTag $tag -Snapshot } 'cannot be read'
}
finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAILED: $_" }
    exit 1
}
Write-Host 'Package source identity: clean-tree and release-tag rules hold.'
exit 0
