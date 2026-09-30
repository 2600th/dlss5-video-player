[CmdletBinding()]
param(
    [string]$DestinationDirectory = '',
    # The archive is pinned by SHA-256, so a copy of the same bytes anywhere -
    # a local file:// path, another mirror - can be named here instead of the
    # project's mirror and BtbN's original.
    [string]$ArchiveUrl = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProgressPreference = 'SilentlyContinue'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $DestinationDirectory) {
    $DestinationDirectory = Join-Path $repositoryRoot 'external\ffmpeg\bin'
}
# BtbN's GPL build of the FFmpeg 9.0 branch (P1.34). Its build scripts are
# public and pin every library's source (FFmpeg-Builds at the autobuild tag's
# commit, 6c9aec5fc9a72ec3abedd1fa84db141fa18cf52b), so the Corresponding
# Source GPLv3 asks for is public upstream (THIRD_PARTY_LICENSES/ffmpeg.txt).
# gyan.dev, the previous build, publishes neither its build scripts nor its
# libraries' sources.
$archiveName = 'ffmpeg-n9.0.2-14-gebafaee10a-win64-gpl-9.0'
# BtbN prunes its autobuild releases after a while, so the project keeps the
# same bytes as a release asset and tries that first.
$archiveUrls = if ($ArchiveUrl) { @($ArchiveUrl) } else {
    @(
        "https://github.com/2600th/dlss5-video-player/releases/download/deps-ffmpeg-n9.0.2-14/$archiveName.zip",
        "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-29-13-10/$archiveName.zip"
    )
}
$archiveSha256 = '11F676F2EE62C39768CEF1892E2E171ADF00FD04C85620B771520E985E7C7A69'
$lock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\tool-lock.json') -Raw | ConvertFrom-Json
$helpers = @($lock.entries | Where-Object { $_.name -in @('ffmpeg.exe', 'ffprobe.exe') })
if ($helpers.Count -ne 2) { throw 'The tool lock must contain FFmpeg and FFprobe.' }

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('dlss-player-ffmpeg-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $archivePath = Join-Path $temporaryRoot 'ffmpeg.zip'
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $downloaded = $false
    foreach ($url in $archiveUrls) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $archivePath -UseBasicParsing -TimeoutSec 600
            $downloaded = $true
            break
        } catch {
            Write-Warning "FFmpeg archive unavailable from ${url}: $($_.Exception.Message)"
        }
    }
    if (-not $downloaded) { throw 'The FFmpeg archive could not be downloaded from any source.' }
    if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -cne $archiveSha256) {
        throw 'FFmpeg archive SHA-256 mismatch.'
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        foreach ($helper in $helpers) {
            $entry = $archive.GetEntry("$archiveName/bin/" + $helper.name)
            if (-not $entry) { throw "Archive is missing $($helper.name)." }
            $staged = Join-Path $temporaryRoot $helper.name
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $staged)
            if ((Get-Item -LiteralPath $staged).Length -ne $helper.size -or
                (Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash -cne $helper.sha256) {
                throw "Tool lock mismatch for $($helper.name)."
            }
        }
    }
    finally { $archive.Dispose() }

    # Verify both inputs before replacing either destination helper.
    New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
    foreach ($helper in $helpers) {
        Copy-Item -LiteralPath (Join-Path $temporaryRoot $helper.name) -Destination (Join-Path $DestinationDirectory $helper.name) -Force
    }
    Write-Host "Verified and staged FFmpeg/FFprobe n9.0.2-14-gebafaee10a in '$DestinationDirectory'."
}
finally {
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $temporaryFull = [IO.Path]::GetFullPath($temporaryRoot)
    if (-not $temporaryFull.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe temporary cleanup path.'
    }
    if (Test-Path -LiteralPath $temporaryFull) { Remove-Item -LiteralPath $temporaryFull -Recurse -Force }
}
