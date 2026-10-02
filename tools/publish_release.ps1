[CmdletBinding()]
param(
    # The complete package package_release.ps1 assembled. Defaults to the
    # -PackageSuffix '' output for this VERSION, then to the -upscaling one.
    [string]$Package,
    # Also publish the draft. Without it the files are attached and the draft is
    # left for a last look on GitHub.
    [switch]$Publish
)

# The release's last steps on the maintainer's machine, after the tag workflow
# has made the draft and package_release.ps1 has assembled the complete package
# from a build the hardware suite passed on:
#
#   1. verify the package against the allowlist, the manifest and the locks;
#   2. name it as README tells people to download it,
#      dlss5-video-player-v<version>-win64.zip - renamed in place, so dist\
#      does not hold two copies of a 378 MB file;
#   3. write its .sha256 in the form `sha256sum -c` reads;
#   4. attach both to the draft release for this VERSION, refusing to replace
#      an asset already there;
#   5. with -Publish, publish the draft. attest-release-asset.yml then attests
#      the published zip against that .sha256 by itself.
#
# It needs the GitHub CLI, signed in with permission to edit releases.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw "VERSION must read major.minor.patch; received '$version'." }
$tag = "dlss5-video-player-v$version"
$distRoot = Join-Path $repositoryRoot 'dist'
$assetName = "$tag-win64.zip"
$assetPath = Join-Path $distRoot $assetName

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'The GitHub CLI (gh) is not on PATH.' }

# The draft has to exist and still be a draft: publishing twice, or attaching to
# a release people may already be downloading, is not this script's job.
# Windows PowerShell turns a native command's stderr into an error record, which
# 'Stop' would throw on before the exit code could be read.
$ErrorActionPreference = 'Continue'
$releaseJson = & gh release view $tag --json 'isDraft,assets' 2>$null
$viewed = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($viewed -ne 0 -or -not $releaseJson) {
    throw "There is no release for $tag. Push the tag and let release-windows create the draft first."
}
$release = $releaseJson | ConvertFrom-Json
if (-not $release.isDraft) { throw "Release $tag is already published; attach or replace its files by hand if that is really meant." }
$attached = @(@($release.assets) | Where-Object { $_.name -in @($assetName, "$assetName.sha256") } | ForEach-Object { $_.name })
if ($attached.Count -eq 2) {
    # An earlier run attached both and left the draft for a look.
    if (-not $Publish) { Write-Host "$assetName and its .sha256 are already attached to the draft $tag."; return }
    & gh release edit $tag --draft=false
    if ($LASTEXITCODE -ne 0) { throw "Publishing $tag failed." }
    Write-Host "Published $tag. attest-release-asset attests $assetName now; watch it with: gh run list --workflow attest-release-asset.yml"
    return
}
if ($attached.Count -eq 1) {
    throw "Release $tag has $($attached[0]) without its partner; remove it on GitHub and run this again."
}

if (-not $Package) {
    if (Test-Path -LiteralPath $assetPath -PathType Leaf) {
        $Package = $assetPath
    } else {
        foreach ($candidate in @("DLSSVideoPlayer-v$version-win64.zip", "DLSSVideoPlayer-v$version-upscaling-win64.zip")) {
            $path = Join-Path $distRoot $candidate
            if (Test-Path -LiteralPath $path -PathType Leaf) { $Package = $path; break }
        }
    }
    if (-not $Package) { throw "No complete package for $version in dist\. Run package_release.bat first." }
}
$Package = (Resolve-Path -LiteralPath $Package).Path

# The verifier is told the suffix the packager used, which is in the file name
# until the rename below.
$packageName = [IO.Path]::GetFileName($Package)
if ($packageName -ne $assetName) {
    $suffix = if ($packageName -like '*-upscaling-win64.zip') { '-upscaling' } else { '' }
    & (Join-Path $PSScriptRoot 'verify_package.ps1') -Zip $Package -PackageSuffix $suffix
    if ($LASTEXITCODE -ne 0) { throw "verify_package.ps1 refused $packageName." }
    if (Test-Path -LiteralPath $assetPath) { throw "$assetPath already exists; remove it or pass -Package." }
    Move-Item -LiteralPath $Package -Destination $assetPath
    Write-Host "Verified $packageName and renamed it $assetName."
} else {
    Write-Host "$assetName was renamed by an earlier run; it was verified then."
}

# '<hash>  <name>' and a bare LF: the only line sha256sum -c accepts.
$hash = (Get-FileHash -LiteralPath $assetPath -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$assetPath.sha256"
[IO.File]::WriteAllText($checksumPath, "$hash  $assetName`n")
Write-Host "sha256 $hash"

& gh release upload $tag $assetPath $checksumPath
if ($LASTEXITCODE -ne 0) { throw "Uploading to $tag failed." }
Write-Host "Attached $assetName and its .sha256 to the draft $tag."

if ($Publish) {
    & gh release edit $tag --draft=false
    if ($LASTEXITCODE -ne 0) { throw "Publishing $tag failed; the files are attached to the draft." }
    Write-Host "Published $tag. attest-release-asset attests $assetName now; watch it with: gh run list --workflow attest-release-asset.yml"
} else {
    Write-Host "Draft left unpublished. Look it over on GitHub, then run this again with -Publish, or publish it there."
}
