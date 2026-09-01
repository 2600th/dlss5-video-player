[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InputDirectory,
    [Parameter(Mandatory = $true)][string]$Destination,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$lockPath = Join-Path $repositoryRoot 'packaging\runtime-lock.json'
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
$inputRoot = (Resolve-Path -LiteralPath $InputDirectory).Path
$destinationRoot = if ([IO.Path]::IsPathRooted($Destination)) {
    [IO.Path]::GetFullPath($Destination)
}
else {
    [IO.Path]::GetFullPath((Join-Path (Get-Location) $Destination))
}

function Get-NormalizedVersion {
    param([Parameter(Mandatory = $true)][string]$Path)
    return ([Diagnostics.FileVersionInfo]::GetVersionInfo($Path).FileVersion -replace ',', '.').Trim()
}

function Assert-LockedFile {
    param(
        [Parameter(Mandatory = $true)]$Entry,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $item = Get-Item -LiteralPath $Path
    if ($item.Length -ne [int64]$Entry.size) {
        throw "Size mismatch for '$($Entry.sourceName)': expected $($Entry.size), received $($item.Length)."
    }

    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($hash -cne [string]$Entry.sha256) {
        throw "SHA-256 mismatch for '$($Entry.sourceName)': expected $($Entry.sha256), received $hash."
    }

    $version = Get-NormalizedVersion -Path $Path
    if ($version -cne [string]$Entry.fileVersion) {
        throw "Version mismatch for '$($Entry.sourceName)': expected $($Entry.fileVersion), received $version."
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ([string]$signature.Status -cne [string]$Entry.signatureStatus) {
        throw "Authenticode mismatch for '$($Entry.sourceName)': expected $($Entry.signatureStatus), received $($signature.Status)."
    }
    if ([string]$Entry.signerContains) {
        $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
        if ($subject -notlike "*$($Entry.signerContains)*") {
            throw "Signer mismatch for '$($Entry.sourceName)': expected '$($Entry.signerContains)', received '$subject'."
        }
    }

    [pscustomobject]@{
        Source = $Path
        Destination = [string]$Entry.destination
        Size = $item.Length
        SHA256 = $hash
        Authenticode = [string]$signature.Status
    }
}

$verified = foreach ($entry in $lock.entries) {
    $matches = @(Get-ChildItem -LiteralPath $inputRoot -Recurse -File -Filter ([string]$entry.sourceName))
    if ($matches.Count -eq 0 -and [string]$entry.sourceName -cne [string]$entry.destination) {
        $matches = @(Get-ChildItem -LiteralPath $inputRoot -Recurse -File -Filter ([string]$entry.destination))
    }
    if ($matches.Count -ne 1) {
        throw "Expected exactly one '$($entry.sourceName)' or staged '$($entry.destination)' below '$inputRoot'; found $($matches.Count)."
    }
    Assert-LockedFile -Entry $entry -Path $matches[0].FullName
}

if (-not $ValidateOnly) {
    New-Item -ItemType Directory -Path $destinationRoot -Force | Out-Null
    foreach ($file in $verified) {
        Copy-Item -LiteralPath $file.Source -Destination (Join-Path $destinationRoot $file.Destination) -Force
    }
}

$verified | Sort-Object Destination | Format-Table Destination, Size, SHA256, Authenticode -AutoSize
if ($ValidateOnly) {
    Write-Host "Verified $($verified.Count) locked runtime files in '$inputRoot'."
}
else {
    Write-Host "Verified and staged $($verified.Count) locked runtime files into '$destinationRoot'."
}
