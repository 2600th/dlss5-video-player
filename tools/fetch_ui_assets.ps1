[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$packageName = '@tabler/icons-webfont'
$packageVersion = '3.46.0'
$packageUrl = 'https://registry.npmjs.org/@tabler/icons-webfont/-/icons-webfont-3.46.0.tgz'
$expectedSri = 'sha512-aQouIxJQb+F5cRsHo/FW5qSILDuU7pd7d86JjmSUCMgpJhBeRuyovnfJlQeyuug2gHz0jFaosl4GNYo3SLnjrQ=='
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repositoryRoot 'assets\tabler'
$destinationParent = Split-Path -Parent $destination
$transactionId = [Guid]::NewGuid().ToString('N')
$stagedDestination = Join-Path $destinationParent (".tabler-stage-{0}" -f $transactionId)
$backupDestination = Join-Path $destinationParent (".tabler-backup-{0}" -f $transactionId)
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("dlss-player-tabler-{0}" -f [Guid]::NewGuid().ToString('N'))
$archive = Join-Path $temporaryRoot 'icons-webfont.tgz'
$extracted = Join-Path $temporaryRoot 'extracted'
$destinationBackedUp = $false

function Remove-ScopedDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($fullParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a directory outside '$fullParent': $fullPath"
    }
    [IO.Directory]::Delete($fullPath, $true)
}

try {
    New-Item -ItemType Directory -Path $temporaryRoot, $extracted -Force | Out-Null
    Invoke-WebRequest -Uri $packageUrl -OutFile $archive -UseBasicParsing

    $sha512 = [Security.Cryptography.SHA512]::Create()
    try {
        $stream = [IO.File]::OpenRead($archive)
        try {
            $digest = $sha512.ComputeHash($stream)
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $sha512.Dispose()
    }

    $actualSri = 'sha512-' + [Convert]::ToBase64String($digest)
    if ($actualSri -cne $expectedSri) {
        throw "Integrity check failed for $packageName $packageVersion. Expected '$expectedSri' but received '$actualSri'."
    }

    & tar.exe -xzf $archive -C $extracted
    if ($LASTEXITCODE -ne 0) {
        throw "tar.exe failed with exit code $LASTEXITCODE."
    }

    $packageRoot = Join-Path $extracted 'package'
    $sourceFiles = @{
        'tabler-icons.ttf' = Join-Path $packageRoot 'dist\fonts\tabler-icons.ttf'
        'tabler-icons.css' = Join-Path $packageRoot 'dist\tabler-icons.css'
        'LICENSE' = Join-Path $packageRoot 'LICENSE'
    }
    foreach ($source in $sourceFiles.Values) {
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Expected package asset is missing: $source"
        }
    }

    New-Item -ItemType Directory -Path $destinationParent, $stagedDestination -Force | Out-Null
    foreach ($name in $sourceFiles.Keys) {
        Copy-Item -LiteralPath $sourceFiles[$name] -Destination (Join-Path $stagedDestination $name) -Force
    }

    $sourceText = @"
Package: $packageName
Version: $packageVersion
Source: $packageUrl
Integrity: $expectedSri
License: MIT (see LICENSE)
Committed files: dist/fonts/tabler-icons.ttf, dist/tabler-icons.css, LICENSE
Application icon derivative: Tabler sparkles glyph (U+F6D7), rendered blue on charcoal as 16, 24, 32, 48, and 256 px ICO frames with Pillow.
"@
    $sourceText += [Environment]::NewLine
    $utf8NoBom = New-Object System.Text.UTF8Encoding -ArgumentList $false
    [IO.File]::WriteAllText((Join-Path $stagedDestination 'SOURCE.txt'), $sourceText, $utf8NoBom)

    $stagedNames = @(Get-ChildItem -LiteralPath $stagedDestination -File | Select-Object -ExpandProperty Name | Sort-Object)
    $expectedNames = @('LICENSE', 'SOURCE.txt', 'tabler-icons.css', 'tabler-icons.ttf')
    if ([string]::Join('|', $stagedNames) -cne [string]::Join('|', $expectedNames)) {
        throw "Staged output was not the expected four-file set: $([string]::Join(', ', $stagedNames))"
    }

    try {
        if (Test-Path -LiteralPath $destination) {
            [IO.Directory]::Move($destination, $backupDestination)
            $destinationBackedUp = $true
        }
        [IO.Directory]::Move($stagedDestination, $destination)
    }
    catch {
        if ($destinationBackedUp) {
            Remove-ScopedDirectory -Path $destination -Parent $destinationParent
            [IO.Directory]::Move($backupDestination, $destination)
            $destinationBackedUp = $false
        }
        throw
    }

    if ($destinationBackedUp) {
        Remove-ScopedDirectory -Path $backupDestination -Parent $destinationParent
        $destinationBackedUp = $false
    }

    Write-Host "Verified and staged $packageName $packageVersion from $packageUrl"
}
finally {
    Remove-ScopedDirectory -Path $temporaryRoot -Parent ([IO.Path]::GetTempPath())
    Remove-ScopedDirectory -Path $stagedDestination -Parent $destinationParent
    if ($destinationBackedUp -and -not (Test-Path -LiteralPath $destination)) {
        [IO.Directory]::Move($backupDestination, $destination)
        $destinationBackedUp = $false
    }
    if (Test-Path -LiteralPath $backupDestination) {
        Remove-ScopedDirectory -Path $backupDestination -Parent $destinationParent
    }
}
