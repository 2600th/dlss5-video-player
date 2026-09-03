[CmdletBinding()]
param([string]$DestinationDirectory = '')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProgressPreference = 'SilentlyContinue'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $DestinationDirectory) {
    $DestinationDirectory = Join-Path $repositoryRoot 'external\ffmpeg\bin'
}
$archiveUrl = 'https://github.com/GyanD/codexffmpeg/releases/download/9.0.1/ffmpeg-9.0.1-essentials_build.zip'
$archiveSha256 = 'FEC81AE03971D9DD4BE3EBE02E263BD2EC1D789483F931BDBA5F5715E65DA2E9'
$lock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\tool-lock.json') -Raw | ConvertFrom-Json
$helpers = @($lock.entries | Where-Object { $_.name -in @('ffmpeg.exe', 'ffprobe.exe') })
if ($helpers.Count -ne 2) { throw 'The tool lock must contain FFmpeg and FFprobe.' }

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('dlss-player-ffmpeg-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $archivePath = Join-Path $temporaryRoot 'ffmpeg.zip'
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $archiveUrl -OutFile $archivePath -UseBasicParsing -TimeoutSec 300
    if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -cne $archiveSha256) {
        throw 'FFmpeg archive SHA-256 mismatch.'
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        foreach ($helper in $helpers) {
            $entry = $archive.GetEntry('ffmpeg-9.0.1-essentials_build/bin/' + $helper.name)
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
    Write-Host "Verified and staged FFmpeg/FFprobe 9.0.1 in '$DestinationDirectory'."
}
finally {
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $temporaryFull = [IO.Path]::GetFullPath($temporaryRoot)
    if (-not $temporaryFull.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe temporary cleanup path.'
    }
    if (Test-Path -LiteralPath $temporaryFull) { Remove-Item -LiteralPath $temporaryFull -Recurse -Force }
}
