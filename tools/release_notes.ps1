[CmdletBinding()]
param(
    # owner/name, for the CHANGELOG link at the tag.
    [Parameter(Mandatory = $true)][string]$Repository,
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The release body was the generated commit list, which says what was committed
# rather than what changed for someone installing it. The CHANGELOG section is
# the other extreme - 470 lines for 0.22.0, which buries the download. README's
# "What changed" paragraph for this version is the short version the repository
# already maintains, so that is the body, with the full section one link away.
#
# The release workflow runs this to publish; the build workflow runs it on a
# pull request that changes VERSION, so both guards below fail the PR rather
# than the tag push.
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
$tag = "dlss5-video-player-v$version"
if (-not $OutFile) { $OutFile = Join-Path $repositoryRoot 'release-notes.md' }
$OutFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)

# The cut still has to have happened: a tag pushed while the section is still
# called Unreleased is the mistake this catches.
if (-not (Select-String -LiteralPath (Join-Path $repositoryRoot 'CHANGELOG.md') -Pattern "^## $([regex]::Escape($version))( |`$)" -Quiet)) {
    throw "CHANGELOG.md has no '## $version' section; cut it before tagging."
}

$lines = Get-Content -LiteralPath (Join-Path $repositoryRoot 'README.md')
$start = $null
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match "^\*\*$([regex]::Escape($version))\*\* \(") { $start = $i; break }
}
if ($null -eq $start) {
    throw "README.md has no '**$version** (date)' entry under What changed; write it before tagging."
}
$end = $lines.Count
for ($i = $start + 1; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\*\*[0-9]+\.[0-9]+\.[0-9]+\*\* \(') { $end = $i; break }
}
$summary = ($lines[$start..($end - 1)] -join "`n").TrimEnd()

# CI builds, tests and attests the core zip. The complete zip with the neural
# runtime is what the README tells people to download, and CI cannot fetch that
# runtime, so the release is created as a draft and the maintainer attaches the
# complete zip before publishing - which is why the intro can promise it.
$intro = @(
    'Windows x64, an RTX GPU and NVIDIA driver 610.47 or newer. Community project, not an',
    'NVIDIA product; the neural runtime is a modified, unsigned community build.',
    '',
    ('Download `dlss5-video-player-v{0}-win64.zip` below - player plus the pinned neural' -f $version),
    'runtime, assembled by the maintainer and attached before this release was published.',
    ('`DLSSVideoPlayer-v{0}-core-win64.zip` is the player alone, with no runtime in it,' -f $version),
    'built and attested by CI. Each has a `.sha256` beside it; GitHub''s "Source code" zip',
    'does not run.',
    ''
)
$tail = @(
    '',
    ('Every detail, including what was measured: [CHANGELOG.md for {0}](https://github.com/{1}/blob/{2}/CHANGELOG.md).' -f $version, $Repository, $tag)
)
$body = (($intro + $summary + $tail) -join "`n").TrimEnd() + "`n"
[IO.File]::WriteAllText($OutFile, $body)
Write-Host "Release notes: $($body.Length) characters from README's $version entry, written to $OutFile."
