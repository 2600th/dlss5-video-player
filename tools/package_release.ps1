[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
if ($version -cne '0.12.0') { throw "This assembler is locked to release 0.12.0; VERSION is '$version'." }

$distRoot = Join-Path $repositoryRoot 'dist'
$stageName = "DLSSVideoPlayer-v$version-win64"
$stageRoot = Join-Path $distRoot $stageName
$zipPath = Join-Path $distRoot "$stageName.zip"
$buildRoot = Join-Path $repositoryRoot 'build\Release'
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

function Assert-HashLock {
    param([string]$Root, [string]$LockPath, [string]$NameProperty)
    $lock = Get-Content -LiteralPath $LockPath -Raw | ConvertFrom-Json
    foreach ($entry in $lock.entries) {
        $name = [string]$entry.$NameProperty
        $path = Join-Path $Root $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Locked input is missing: $path" }
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

& (Join-Path $PSScriptRoot 'stage_runtime.ps1') -InputDirectory $runtimeRoot -Destination $runtimeRoot -ValidateOnly | Out-Host
Assert-HashLock -Root $runtimeRoot -LockPath (Join-Path $repositoryRoot 'packaging\runtime-lock.json') -NameProperty 'destination'

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

$sources = @(
    @('DLSSVideoPlayer.exe', (Join-Path $buildRoot 'DLSSVideoPlayer.exe')),
    @('ffmpeg.exe', (Join-Path $ffmpegRoot 'ffmpeg.exe')),
    @('ffprobe.exe', (Join-Path $ffmpegRoot 'ffprobe.exe')),
    @('yt-dlp.exe', (Join-Path $youtubeRoot 'yt-dlp.exe')),
    @('deno.exe', (Join-Path $youtubeRoot 'deno.exe')),
    @('dxgi.dll', (Join-Path $runtimeRoot 'dxgi.dll')),
    @('ReShade.ini', (Join-Path $repositoryRoot 'packaging\ReShade.ini')),
    @('ReShadePreset.ini', (Join-Path $repositoryRoot 'packaging\ReShadePreset.ini')),
    @('renodx-dlss5.addon64', (Join-Path $runtimeRoot 'renodx-dlss5.addon64')),
    @('nvngx_dlss.dll', (Join-Path $runtimeRoot 'nvngx_dlss.dll')),
    @('nvngx_dlssnr.dll', (Join-Path $runtimeRoot 'nvngx_dlssnr.dll')),
    @('sl.common.dll', (Join-Path $runtimeRoot 'sl.common.dll')),
    @('sl.dlss.dll', (Join-Path $runtimeRoot 'sl.dlss.dll')),
    @('sl.dlss_g.dll', (Join-Path $runtimeRoot 'sl.dlss_g.dll')),
    @('sl.dlss_nr.dll', (Join-Path $runtimeRoot 'sl.dlss_nr.dll')),
    @('sl.interposer.dll', (Join-Path $runtimeRoot 'sl.interposer.dll')),
    @('sl.nis.dll', (Join-Path $runtimeRoot 'sl.nis.dll')),
    @('sl.pcl.dll', (Join-Path $runtimeRoot 'sl.pcl.dll')),
    @('sl.reflex.dll', (Join-Path $runtimeRoot 'sl.reflex.dll')),
    @('README.md', (Join-Path $repositoryRoot 'README.md')),
    @('LICENSE', (Join-Path $repositoryRoot 'LICENSE')),
    @('THIRD_PARTY.md', (Join-Path $repositoryRoot 'THIRD_PARTY.md')),
    @('THIRD_PARTY_LICENSES/yt-dlp-2026.08.19.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\yt-dlp-2026.08.19.txt')),
    @('THIRD_PARTY_LICENSES/deno-2.9.5.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\deno-2.9.5.txt')),
    @('THIRD_PARTY_LICENSES/ffmpeg.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\ffmpeg.txt')),
    @('THIRD_PARTY_LICENSES/experimental-runtime.txt', (Join-Path $repositoryRoot 'THIRD_PARTY_LICENSES\experimental-runtime.txt')),
    @('THIRD_PARTY_LICENSES/tabler-MIT.txt', (Join-Path $repositoryRoot 'assets\tabler\LICENSE')),
    @('docs/ARCHITECTURE.md', (Join-Path $repositoryRoot 'docs\ARCHITECTURE.md')),
    @('docs/BUILDING.md', (Join-Path $repositoryRoot 'docs\BUILDING.md')),
    @('docs/DLSS5_SETUP.md', (Join-Path $repositoryRoot 'docs\DLSS5_SETUP.md')),
    @('docs/TROUBLESHOOTING.md', (Join-Path $repositoryRoot 'docs\TROUBLESHOOTING.md')),
    @('EXPERIMENTAL_RUNTIME_NOTICE.txt', (Join-Path $repositoryRoot 'packaging\EXPERIMENTAL_RUNTIME_NOTICE.txt'))
)

foreach ($source in $sources) {
    if (-not (Test-Path -LiteralPath $source[1] -PathType Leaf)) { throw "Required package input is missing: $($source[1])" }
}

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
$distFull = [IO.Path]::GetFullPath($distRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$stageFull = [IO.Path]::GetFullPath($stageRoot)
if (-not $stageFull.StartsWith($distFull, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe stage path.' }
if (Test-Path -LiteralPath $stageFull) { [IO.Directory]::Delete($stageFull, $true) }
New-Item -ItemType Directory -Path $stageFull | Out-Null

foreach ($source in $sources) {
    $destination = Join-Path $stageFull $source[0]
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source[1] -Destination $destination
}

$files = @(Get-ChildItem -LiteralPath $stageFull -Recurse -File | Sort-Object {
    Get-RelativePackagePath -Root $stageFull -Path $_.FullName
})
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

& (Join-Path $PSScriptRoot 'verify_package.ps1') -StageDirectory $stageFull

if (Test-Path -LiteralPath $zipPath) { [IO.File]::Delete($zipPath) }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $stageFull -Recurse -File | Sort-Object {
        Get-RelativePackagePath -Root $stageFull -Path $_.FullName
    }) {
        $relative = Get-RelativePackagePath -Root $stageFull -Path $file.FullName
        $entry = "$stageName/$relative"
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
}
finally { $archive.Dispose() }

& (Join-Path $PSScriptRoot 'verify_package.ps1') -Zip $zipPath
$zipHash = Get-Sha256 -Path $zipPath
Write-Host "Created '$zipPath' ($((Get-Item -LiteralPath $zipPath).Length) bytes, SHA-256 $zipHash)."
