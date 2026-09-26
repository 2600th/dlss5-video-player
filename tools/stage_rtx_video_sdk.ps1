[CmdletBinding()]
param(
    # The archive NVIDIA serves at
    # https://developer.nvidia.com/downloads/rtx/sdk/rtx_video_sdk_v1.1.0.zip
    # to a signed-in NVIDIA developer account (the browser may name it
    # RTX_Video_SDK_v1.1.0.zip).
    [Parameter(Mandatory = $true)][string]$Archive
)

# Stages the NVIDIA RTX Video SDK into external/rtx-video-sdk, where CMake finds
# it without being told (docs/BUILDING.md#rtx-video-super-resolution-optional).
#
# The SDK cannot be fetched by a script or kept in this repository: the download
# needs an NVIDIA developer login, and its licence permits distribution only as
# object code inside an application (THIRD_PARTY.md). So each developer
# downloads it once from NVIDIA, and this script checks that the archive is the
# one this project was built and tested against before extracting it: the
# archive's SHA-256, then NVIDIA's signature and file version on the
# nvngx_vsr.dll the player ships.
#
# NVIDIA withdrew the SDK's public page in 2026 (developer.nvidia.com/rtx-video-sdk
# now redirects elsewhere); the archive URL above still answered on 2026-09-26.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$expectedSha256 = 'ABF4F34E2B5A618E355B0D5A0365D8ECC3DB4396E756E4C850A867E1AE2ED69E'
$expectedVsrVersion = '1.6.0.0'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repositoryRoot 'external\rtx-video-sdk'
$staging = Join-Path $repositoryRoot ('external\.rtx-video-sdk-stage-{0}' -f [Guid]::NewGuid().ToString('N'))

$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
if ($hash -cne $expectedSha256) {
    throw "The archive's SHA-256 is $hash, not the $expectedSha256 of RTX Video SDK 1.1.0 this project is tested against. Download it again from NVIDIA; a different SDK version needs the pin here updated after testing."
}

try {
    Expand-Archive -LiteralPath $archivePath -DestinationPath $staging
    $header = Join-Path $staging 'include\nvsdk_ngx_helpers_vsr.h'
    $vsr = Join-Path $staging 'bin\Windows\x64\rel\nvngx_vsr.dll'
    foreach ($required in @($header, $vsr)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "The archive has no $required." }
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $vsr
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=NVIDIA Corporation') {
        throw "nvngx_vsr.dll is not validly signed by NVIDIA (status $($signature.Status))."
    }
    $version = (Get-Item -LiteralPath $vsr).VersionInfo.FileVersion -replace ',', '.' -replace ' ', ''
    if ($version -cne $expectedVsrVersion) {
        throw "nvngx_vsr.dll is version $version, not $expectedVsrVersion."
    }

    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    Move-Item -LiteralPath $staging -Destination $destination
}
finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}

Write-Output "Staged RTX Video SDK 1.1.0 (nvngx_vsr.dll $expectedVsrVersion, NVIDIA-signed) in '$destination'."
Write-Output 'Configure again and CMake builds the RTX VSR view: "NVIDIA RTX Video SDK found at ..." in its output.'
