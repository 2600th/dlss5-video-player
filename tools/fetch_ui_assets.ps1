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
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("dlss-player-tabler-{0}" -f [Guid]::NewGuid().ToString('N'))
$archive = Join-Path $temporaryRoot 'icons-webfont.tgz'
$extracted = Join-Path $temporaryRoot 'extracted'

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

    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    foreach ($name in $sourceFiles.Keys) {
        Copy-Item -LiteralPath $sourceFiles[$name] -Destination (Join-Path $destination $name) -Force
    }

    $sourceText = @"
Package: $packageName
Version: $packageVersion
Source: $packageUrl
Integrity: $expectedSri
License: MIT (see LICENSE)
Committed files: dist/fonts/tabler-icons.ttf, dist/tabler-icons.css, LICENSE
"@
    Set-Content -LiteralPath (Join-Path $destination 'SOURCE.txt') -Value $sourceText -Encoding utf8NoBOM

    Write-Host "Verified and staged $packageName $packageVersion from $packageUrl"
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
