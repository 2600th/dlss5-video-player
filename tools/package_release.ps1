[CmdletBinding()]
param(
    [switch]$ValidateBuildOnly,
    [switch]$PublicCore,
    [string]$BuildDirectory = 'build-upscaling',
    [string]$PackageSuffix = '-upscaling'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
# The assembler used to be pinned to one literal release, which meant every
# version bump failed the release workflow at the packaging step until someone
# edited this line. VERSION is the single source now; the shape is still
# checked, and the built executable's own version resource is compared against
# it below, which is the property the pin was actually protecting.
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "VERSION must read major.minor.patch; received '$version'."
}

$distRoot = Join-Path $repositoryRoot 'dist'
$stageName = if ($PublicCore) { "DLSSVideoPlayer-v$version-core-win64" } else { "DLSSVideoPlayer-v$version$PackageSuffix-win64" }
$stageRoot = Join-Path $distRoot $stageName
$zipPath = Join-Path $distRoot "$stageName.zip"
$buildRoot = Join-Path (Join-Path $repositoryRoot $BuildDirectory) 'Release'
$runtimeRoot = Join-Path $repositoryRoot 'external\runtime'
$youtubeRoot = Join-Path $repositoryRoot 'external\youtube'
$ffmpegRoot = Join-Path $repositoryRoot 'external\ffmpeg\bin'

function Get-RelativePackagePath {
    param([string]$Root, [string]$Path)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $pathFull = [IO.Path]::GetFullPath($Path)
    if (-not $pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside package root: $pathFull"
    }
    return $pathFull.Substring($rootFull.Length).Replace('\', '/')
}

function Get-Sha256 {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '') }
        finally { $sha256.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Get-OrdinalPackageFiles {
    param([string]$Root)
    $filesByPath = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        $relative = Get-RelativePackagePath -Root $Root -Path $file.FullName
        $filesByPath[$relative] = $file
    }
    [string[]]$relativePaths = @($filesByPath.Keys)
    [Array]::Sort($relativePaths, [StringComparer]::Ordinal)
    foreach ($relative in $relativePaths) { $filesByPath[$relative] }
}

# This used to clean the build directory and rebuild the player, so the
# executable it shipped was never the one ctest had just run. It packages the
# tested build now and checks that build's identity instead.
function Assert-ReleaseBuild {
    $configuredBuildDirectory = Join-Path $repositoryRoot $BuildDirectory
    $cachePath = Join-Path $configuredBuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "Configured build directory is missing: $cachePath"
    }

    $homeLine = Get-Content -LiteralPath $cachePath | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' } | Select-Object -First 1
    if (-not $homeLine) { throw 'CMakeCache.txt does not identify its source tree.' }
    $configuredHome = $homeLine.Substring($homeLine.IndexOf('=') + 1)
    if ([IO.Path]::GetFullPath($configuredHome).TrimEnd('\', '/') -ine [IO.Path]::GetFullPath($repositoryRoot).TrimEnd('\', '/')) {
        throw "The build directory belongs to a different source tree: $configuredHome"
    }

    $executable = Join-Path $buildRoot 'DLSSVideoPlayer.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Release build is missing: $executable" }

    $identity = (Get-Item -LiteralPath $executable).VersionInfo
    $expectedIdentity = @{
        ProductName = 'DLSS Video Player'
        FileVersion = "$version.0"
        ProductVersion = "$version.0"
        OriginalFilename = 'DLSSVideoPlayer.exe'
    }
    foreach ($name in $expectedIdentity.Keys) {
        if ([string]$identity.$name -cne $expectedIdentity[$name]) {
            throw "Release executable $name mismatch: expected '$($expectedIdentity[$name])', received '$($identity.$name)'."
        }
    }

    foreach ($helper in @('yt-dlp.exe', 'deno.exe')) {
        $source = Join-Path $youtubeRoot $helper
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            $built = Join-Path $buildRoot $helper
            if (-not (Test-Path -LiteralPath $built -PathType Leaf) -or
                (Get-Sha256 -Path $built) -cne (Get-Sha256 -Path $source)) {
                throw "The build did not stage the pinned YouTube helper '$helper'."
            }
        }
    }

    Write-Host "DLSSVideoPlayer $version.0 build verified."
}

$runtimeLock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\runtime-lock.json') -Raw | ConvertFrom-Json

# fetch_neural_runtime.ps1 stages each file under the lock's sourceName (the
# universal NR runtime is nvngx_dlssnr_310.8.SF-v2.dll in external/runtime); a
# hand-staged tree may carry the destination name. Same rule as stage_runtime.ps1.
function Resolve-LockedRuntimeInput {
    param([string]$Destination)
    $entry = @($runtimeLock.entries | Where-Object { [string]$_.destination -ceq $Destination })
    if ($entry.Count -ne 1) { throw "runtime-lock.json has $($entry.Count) entries for '$Destination'." }
    foreach ($name in @([string]$entry[0].sourceName, [string]$entry[0].destination)) {
        $path = Join-Path $runtimeRoot $name
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    throw "Locked input is missing: $(Join-Path $runtimeRoot ([string]$entry[0].sourceName))"
}

function Assert-HashLock {
    foreach ($entry in $runtimeLock.entries) {
        $name = [string]$entry.destination
        $path = Resolve-LockedRuntimeInput -Destination $name
        $item = Get-Item -LiteralPath $path
        $hash = Get-Sha256 -Path $path
        if ($item.Length -ne [int64]$entry.size -or $hash -cne [string]$entry.sha256) {
            throw "Locked input drifted: $name"
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        if ([string]$signature.Status -cne [string]$entry.signatureStatus) {
            throw "Authenticode status drifted for '$name'."
        }
        if ($entry.PSObject.Properties.Name -contains 'signerContains' -and [string]$entry.signerContains) {
            $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
            if ($subject -notlike "*$($entry.signerContains)*") { throw "Signer drifted for '$name'." }
        }
    }
}

function Get-AuthenticodeRecord {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $header = New-Object byte[] 2
        $read = $stream.Read($header, 0, 2)
    }
    finally { $stream.Dispose() }
    if ($read -ne 2 -or $header[0] -ne 0x4d -or $header[1] -ne 0x5a) {
        return [pscustomobject]@{ Status = 'N/A'; Signer = '' }
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    return [pscustomobject]@{
        Status = [string]$signature.Status
        Signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
    }
}

Assert-ReleaseBuild
if ($ValidateBuildOnly) { return }

if (-not $PublicCore) {
    & (Join-Path $PSScriptRoot 'stage_runtime.ps1') -InputDirectory $runtimeRoot -Destination $runtimeRoot -ValidateOnly | Out-Host
    Assert-HashLock

    $toolLock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\tool-lock.json') -Raw | ConvertFrom-Json
    foreach ($entry in $toolLock.entries) {
        $root = if ([string]$entry.name -in @('ffmpeg.exe', 'ffprobe.exe')) { $ffmpegRoot } else { $youtubeRoot }
        $path = Join-Path $root ([string]$entry.name)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Locked helper is missing: $path" }
        $item = Get-Item -LiteralPath $path
        $hash = Get-Sha256 -Path $path
        if ($item.Length -ne [int64]$entry.size -or $hash -cne [string]$entry.sha256) {
            throw "Locked helper drifted: $($entry.name)"
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        if ([string]$signature.Status -cne [string]$entry.signatureStatus) {
            throw "Authenticode status drifted for '$($entry.name)'."
        }
        if ($entry.PSObject.Properties.Name -contains 'signerContains' -and [string]$entry.signerContains) {
            $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
            if ($subject -notlike "*$($entry.signerContains)*") { throw "Signer drifted for '$($entry.name)'." }
        }
    }
}

if ($PublicCore) {
    $sources = @(
        @('DLSSVideoPlayer.exe', (Join-Path $buildRoot 'DLSSVideoPlayer.exe')),
        @('nvngx_dlss.dll', (Join-Path $buildRoot 'nvngx_dlss.dll')),
        @('README.md', (Join-Path $repositoryRoot 'TECHNICAL_OVERVIEW.md')),
        @('LICENSE', (Join-Path $repositoryRoot 'LICENSE')),
        @('SECURITY.md', (Join-Path $repositoryRoot 'SECURITY.md')),
        @('CONTRIBUTING.md', (Join-Path $repositoryRoot 'CONTRIBUTING.md')),
        @('CHANGELOG.md', (Join-Path $repositoryRoot 'CHANGELOG.md')),
        @('THIRD_PARTY.md', (Join-Path $repositoryRoot 'THIRD_PARTY.md')),
        @('PUBLIC_RELEASE_NOTICE.txt', (Join-Path $repositoryRoot 'packaging\PUBLIC_RELEASE_NOTICE.txt')),
        @('THIRD_PARTY_LICENSES/NVIDIA-DLSS-SDK.txt', (Join-Path $repositoryRoot 'external\DLSS\LICENSE.txt')),
        @('THIRD_PARTY_LICENSES/dlss5-feeder-MIT.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\dlss5-feeder-MIT.txt')),
        @('THIRD_PARTY_LICENSES/nvidia-optical-flow-MIT.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\nvidia-optical-flow-MIT.txt')),
        @('THIRD_PARTY_LICENSES/tabler-MIT.txt', (Join-Path $repositoryRoot 'assets\tabler\LICENSE')),
        @('docs/ARCHITECTURE.md', (Join-Path $repositoryRoot 'docs\ARCHITECTURE.md')),
        @('docs/BUILDING.md', (Join-Path $repositoryRoot 'docs\BUILDING.md')),
        @('docs/DLSS5_SETUP.md', (Join-Path $repositoryRoot 'docs\DLSS5_SETUP.md')),
        @('docs/RELATED_PROJECTS.md', (Join-Path $repositoryRoot 'docs\RELATED_PROJECTS.md')),
        @('docs/TROUBLESHOOTING.md', (Join-Path $repositoryRoot 'docs\TROUBLESHOOTING.md'))
    )
}
else {
    $sources = @(
        @('DLSSVideoPlayer.exe', (Join-Path $buildRoot 'DLSSVideoPlayer.exe')),
        @('ffmpeg.exe', (Join-Path $ffmpegRoot 'ffmpeg.exe')),
        @('ffprobe.exe', (Join-Path $ffmpegRoot 'ffprobe.exe')),
        @('yt-dlp.exe', (Join-Path $youtubeRoot 'yt-dlp.exe')),
        @('deno.exe', (Join-Path $youtubeRoot 'deno.exe')),
        @('neural-runtime/dxgi.dll', (Resolve-LockedRuntimeInput -Destination 'dxgi.dll')),
        @('neural-runtime/ReShade.ini', (Join-Path $repositoryRoot 'packaging\ReShade.ini')),
        @('neural-runtime/ReShadePreset.ini', (Join-Path $repositoryRoot 'packaging\ReShadePreset.ini')),
        @('neural-runtime/renodx-dlss5.addon64', (Resolve-LockedRuntimeInput -Destination 'renodx-dlss5.addon64')),
        @('nvngx_dlss.dll', (Join-Path $buildRoot 'nvngx_dlss.dll')),
        @('neural-runtime/nvngx_dlss.dll', (Resolve-LockedRuntimeInput -Destination 'nvngx_dlss.dll')),
        @('neural-runtime/NeuralWorker.exe', (Join-Path $buildRoot 'neural-runtime/NeuralWorker.exe')),
        @('neural-runtime/nvngx_dlssnr.dll', (Resolve-LockedRuntimeInput -Destination 'nvngx_dlssnr.dll')),
        @('neural-runtime/sl.common.dll', (Resolve-LockedRuntimeInput -Destination 'sl.common.dll')),
        @('neural-runtime/sl.dlss.dll', (Resolve-LockedRuntimeInput -Destination 'sl.dlss.dll')),
        @('neural-runtime/sl.dlss_g.dll', (Resolve-LockedRuntimeInput -Destination 'sl.dlss_g.dll')),
        @('neural-runtime/sl.dlss_nr.dll', (Resolve-LockedRuntimeInput -Destination 'sl.dlss_nr.dll')),
        @('neural-runtime/sl.interposer.dll', (Resolve-LockedRuntimeInput -Destination 'sl.interposer.dll')),
        @('neural-runtime/sl.nis.dll', (Resolve-LockedRuntimeInput -Destination 'sl.nis.dll')),
        @('neural-runtime/sl.pcl.dll', (Resolve-LockedRuntimeInput -Destination 'sl.pcl.dll')),
        @('neural-runtime/sl.reflex.dll', (Resolve-LockedRuntimeInput -Destination 'sl.reflex.dll')),
        @('README.md', (Join-Path $repositoryRoot 'TECHNICAL_OVERVIEW.md')),
        @('LICENSE', (Join-Path $repositoryRoot 'LICENSE')),
        @('SECURITY.md', (Join-Path $repositoryRoot 'SECURITY.md')),
        @('CONTRIBUTING.md', (Join-Path $repositoryRoot 'CONTRIBUTING.md')),
        @('CHANGELOG.md', (Join-Path $repositoryRoot 'CHANGELOG.md')),
        @('THIRD_PARTY.md', (Join-Path $repositoryRoot 'THIRD_PARTY.md')),
        @('THIRD_PARTY_LICENSES/yt-dlp-2026.08.19.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\yt-dlp-2026.08.19.txt')),
        @('THIRD_PARTY_LICENSES/deno-2.9.5.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\deno-2.9.5.txt')),
        @('THIRD_PARTY_LICENSES/ffmpeg.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\ffmpeg.txt')),
        @('THIRD_PARTY_LICENSES/experimental-runtime.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\experimental-runtime.txt')),
        @('THIRD_PARTY_LICENSES/dlss5-feeder-MIT.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\dlss5-feeder-MIT.txt')),
        @('THIRD_PARTY_LICENSES/nvidia-optical-flow-MIT.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\nvidia-optical-flow-MIT.txt')),
        @('THIRD_PARTY_LICENSES/tabler-MIT.txt', (Join-Path $repositoryRoot 'assets\tabler\LICENSE')),
        @('docs/ARCHITECTURE.md', (Join-Path $repositoryRoot 'docs\ARCHITECTURE.md')),
        @('docs/BUILDING.md', (Join-Path $repositoryRoot 'docs\BUILDING.md')),
        @('docs/DLSS5_SETUP.md', (Join-Path $repositoryRoot 'docs\DLSS5_SETUP.md')),
        @('docs/RELATED_PROJECTS.md', (Join-Path $repositoryRoot 'docs\RELATED_PROJECTS.md')),
        @('docs/TROUBLESHOOTING.md', (Join-Path $repositoryRoot 'docs\TROUBLESHOOTING.md')),
        @('EXPERIMENTAL_RUNTIME_NOTICE.txt', (Join-Path $repositoryRoot 'packaging\EXPERIMENTAL_RUNTIME_NOTICE.txt'))
    )
}

$sources += @(
    @('docs/USAGE.md', (Join-Path $repositoryRoot 'docs\USAGE.md')),
    @('docs/EXAMPLE_VIDEOS.md', (Join-Path $repositoryRoot 'docs\EXAMPLE_VIDEOS.md'))
)

foreach ($source in $sources) {
    if (-not (Test-Path -LiteralPath $source[1] -PathType Leaf)) { throw "Required package input is missing: $($source[1])" }
}

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
$distFull = [IO.Path]::GetFullPath($distRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$stageFull = [IO.Path]::GetFullPath($stageRoot)
if (-not $stageFull.StartsWith($distFull, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe stage path.' }
if (Test-Path -LiteralPath $stageFull) { throw 'Package output already exists. Select a new PackageSuffix.' }
New-Item -ItemType Directory -Path $stageFull | Out-Null

foreach ($source in $sources) {
    $destination = Join-Path $stageFull $source[0]
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source[1] -Destination $destination
}

$files = @(Get-OrdinalPackageFiles -Root $stageFull)
$manifest = New-Object Collections.Generic.List[string]
$manifest.Add("ProductVersion=$version")
$manifest.Add('Path|Size|SHA256|Authenticode|Signer')
foreach ($file in $files) {
    $relative = Get-RelativePackagePath -Root $stageFull -Path $file.FullName
    $hash = Get-Sha256 -Path $file.FullName
    $auth = Get-AuthenticodeRecord -Path $file.FullName
    $manifest.Add("$relative|$($file.Length)|$hash|$($auth.Status)|$($auth.Signer)")
}
$utf8NoBom = New-Object Text.UTF8Encoding $false
[IO.File]::WriteAllLines((Join-Path $stageFull 'PACKAGE_MANIFEST.txt'), $manifest, $utf8NoBom)

& (Join-Path $PSScriptRoot 'verify_package.ps1') -StageDirectory $stageFull -PublicCore:$PublicCore -PackageSuffix $PackageSuffix

if (Test-Path -LiteralPath $zipPath) { throw 'Archive already exists. Select a new PackageSuffix.' }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-OrdinalPackageFiles -Root $stageFull) {
        $relative = Get-RelativePackagePath -Root $stageFull -Path $file.FullName
        $entry = "$stageName/$relative"
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
}
finally { $archive.Dispose() }

& (Join-Path $PSScriptRoot 'verify_package.ps1') -Zip $zipPath -PublicCore:$PublicCore -PackageSuffix $PackageSuffix
$zipHash = Get-Sha256 -Path $zipPath
Write-Host "Created '$zipPath' ($((Get-Item -LiteralPath $zipPath).Length) bytes, SHA-256 $zipHash)."
