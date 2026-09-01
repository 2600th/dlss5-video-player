[CmdletBinding(DefaultParameterSetName = 'Stage')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Stage')][string]$StageDirectory,
    [Parameter(Mandatory = $true, ParameterSetName = 'Zip')][string]$Zip
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
$expected = @(
    'DLSSVideoPlayer.exe', 'ffmpeg.exe', 'ffprobe.exe', 'yt-dlp.exe', 'deno.exe',
    'dxgi.dll', 'ReShade.ini', 'ReShadePreset.ini', 'renodx-dlss5.addon64',
    'nvngx_dlss.dll', 'nvngx_dlssnr.dll', 'sl.common.dll', 'sl.dlss.dll',
    'sl.dlss_g.dll', 'sl.dlss_nr.dll', 'sl.interposer.dll', 'sl.nis.dll',
    'sl.pcl.dll', 'sl.reflex.dll', 'README.md', 'LICENSE', 'THIRD_PARTY.md',
    'THIRD_PARTY_LICENSES/yt-dlp-2026.08.19.txt',
    'THIRD_PARTY_LICENSES/deno-2.9.5.txt', 'THIRD_PARTY_LICENSES/ffmpeg.txt',
    'THIRD_PARTY_LICENSES/experimental-runtime.txt',
    'THIRD_PARTY_LICENSES/tabler-MIT.txt', 'docs/ARCHITECTURE.md',
    'docs/BUILDING.md', 'docs/DLSS5_SETUP.md', 'docs/TROUBLESHOOTING.md',
    'EXPERIMENTAL_RUNTIME_NOTICE.txt', 'PACKAGE_MANIFEST.txt'
)
$temporaryRoot = $null

function Get-RelativePackagePath {
    param([string]$Root, [string]$Path)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $pathFull = [IO.Path]::GetFullPath($Path)
    if (-not $pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside package root: $pathFull"
    }
    return $pathFull.Substring($rootFull.Length).Replace('\', '/')
}

function Get-AuthenticodeRecord {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
    try {
        $header = New-Object byte[] 2
        $read = $stream.Read($header, 0, 2)
    }
    finally {
        $stream.Dispose()
    }
    if ($read -ne 2 -or $header[0] -ne 0x4d -or $header[1] -ne 0x5a) {
        return [pscustomobject]@{ Status = 'N/A'; Signer = '' }
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    return [pscustomobject]@{
        Status = [string]$signature.Status
        Signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
    }
}

function Assert-LockedFiles {
    param([string]$Root)
    $runtimeLock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\runtime-lock.json') -Raw | ConvertFrom-Json
    foreach ($entry in $runtimeLock.entries) {
        $path = Join-Path $Root ([string]$entry.destination)
        $item = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($item.Length -ne [int64]$entry.size -or $hash -cne [string]$entry.sha256) {
            throw "Locked runtime mismatch: $($entry.destination)"
        }
    }
    $toolLock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\tool-lock.json') -Raw | ConvertFrom-Json
    foreach ($entry in $toolLock.entries) {
        $path = Join-Path $Root ([string]$entry.name)
        $item = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($item.Length -ne [int64]$entry.size -or $hash -cne [string]$entry.sha256) {
            throw "Locked helper mismatch: $($entry.name)"
        }
    }
}

function Assert-Stage {
    param([string]$Root)
    $resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
    $actual = @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File | ForEach-Object {
        Get-RelativePackagePath -Root $resolvedRoot -Path $_.FullName
    } | Sort-Object)
    $expectedSorted = @($expected | Sort-Object)
    if ([string]::Join("`n", $actual) -cne [string]::Join("`n", $expectedSorted)) {
        $missing = @($expectedSorted | Where-Object { $_ -cnotin $actual })
        $unexpected = @($actual | Where-Object { $_ -cnotin $expectedSorted })
        throw "Package allowlist mismatch. Missing=[$([string]::Join(', ', $missing))] Unexpected=[$([string]::Join(', ', $unexpected))]"
    }

    $knownExecutables = @('DLSSVideoPlayer.exe', 'ffmpeg.exe', 'ffprobe.exe', 'yt-dlp.exe', 'deno.exe')
    $unexpectedExecutables = @($actual | Where-Object { $_.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase) -and $_ -cnotin $knownExecutables })
    if ($unexpectedExecutables.Count -ne 0) {
        throw "Unexpected launchable executable(s): $([string]::Join(', ', $unexpectedExecutables))"
    }

    foreach ($configName in @('ReShade.ini', 'ReShadePreset.ini')) {
        if ((Get-Item -LiteralPath (Join-Path $resolvedRoot $configName)).Length -eq 0) {
            throw "$configName must not be empty."
        }
    }

    foreach ($relative in $actual) {
        if ($relative -match '(?i)(^|[/_.-])pt-br([/_.-]|$)|languages/') {
            throw "Portuguese/language artifact is forbidden: $relative"
        }
        $path = Join-Path $resolvedRoot $relative
        if ($relative -match '(?i)(\.md|\.txt|\.ini)$' -or $relative -ceq 'LICENSE') {
            $text = Get-Content -LiteralPath $path -Raw
            if ($text -match '(?i)\bpt-br\b|portugu[eê]s') {
                throw "Portuguese content marker is forbidden: $relative"
            }
        }
    }

    Assert-LockedFiles -Root $resolvedRoot

    $manifestPath = Join-Path $resolvedRoot 'PACKAGE_MANIFEST.txt'
    $lines = @(Get-Content -LiteralPath $manifestPath)
    if ($lines.Count -lt 3 -or $lines[0] -cne "ProductVersion=$version" -or
        $lines[1] -cne 'Path|Size|SHA256|Authenticode|Signer') {
        throw 'PACKAGE_MANIFEST.txt has an invalid header.'
    }
    $manifestRows = $lines[2..($lines.Count - 1)]
    $manifestExpected = @($actual | Where-Object { $_ -cne 'PACKAGE_MANIFEST.txt' })
    if ($manifestRows.Count -ne $manifestExpected.Count) {
        throw 'PACKAGE_MANIFEST.txt row count does not match the package.'
    }
    for ($index = 0; $index -lt $manifestExpected.Count; ++$index) {
        $relative = $manifestExpected[$index]
        $path = Join-Path $resolvedRoot $relative
        $item = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
        $auth = Get-AuthenticodeRecord -Path $path
        $expectedLine = "$relative|$($item.Length)|$hash|$($auth.Status)|$($auth.Signer)"
        if ($manifestRows[$index] -cne $expectedLine) {
            throw "Manifest mismatch for '$relative'."
        }
    }

    Write-Host "Verified allowlisted package with $($actual.Count) files at '$resolvedRoot'."
}

try {
    if ($PSCmdlet.ParameterSetName -eq 'Zip') {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zipPath = (Resolve-Path -LiteralPath $Zip).Path
        $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
        try {
            $entryNames = @($archive.Entries | Where-Object { $_.Name } | Select-Object -ExpandProperty FullName)
            $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
            foreach ($name in $entryNames) {
                $normalized = $name.Replace('\', '/')
                if ($normalized.StartsWith('/') -or $normalized -match '(^|/)\.\.(/|$)' -or $normalized.Contains(':')) {
                    throw "Unsafe ZIP entry: $name"
                }
                if (-not $seen.Add($normalized)) { throw "Duplicate ZIP entry: $name" }
            }
        }
        finally {
            $archive.Dispose()
        }
        $temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('dlss-player-package-verify-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
        [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $temporaryRoot)
        $roots = @(Get-ChildItem -LiteralPath $temporaryRoot -Directory)
        if ($roots.Count -ne 1 -or @(Get-ChildItem -LiteralPath $temporaryRoot -File).Count -ne 0) {
            throw 'ZIP must contain exactly one top-level release directory.'
        }
        Assert-Stage -Root $roots[0].FullName
    }
    else {
        Assert-Stage -Root $StageDirectory
    }
}
finally {
    if ($temporaryRoot -and (Test-Path -LiteralPath $temporaryRoot)) {
        [IO.Directory]::Delete($temporaryRoot, $true)
    }
}
