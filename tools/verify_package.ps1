# Three ways in. package_release.ps1 calls it from the repository with
# -StageDirectory or -Zip, where VERSION, packaging/*.json and the pinned SDK
# sit beside tools/. A user runs the copy that ships inside the package, where
# none of that exists: that is package mode. It is detected from the layout
# rather than asked for, and it takes the version and the core/complete
# variant from PACKAGE_MANIFEST.txt, so the user needs no switches. This file
# used to read VERSION from its parent directory unconditionally, so the
# command README gave users failed before it checked anything.
[CmdletBinding(DefaultParameterSetName = 'Stage')]
param(
    # Required in the repository. In package mode it defaults to the folder
    # this script was unpacked into.
    [Parameter(ParameterSetName = 'Stage')][string]$StageDirectory,
    [Parameter(Mandatory = $true, ParameterSetName = 'Zip')][string]$Zip,
    [switch]$PublicCore,
    [string]$PackageSuffix = '-upscaling'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$repositoryMode = (Test-Path -LiteralPath (Join-Path $repositoryRoot 'VERSION') -PathType Leaf) -and
    (Test-Path -LiteralPath (Join-Path $repositoryRoot 'packaging\runtime-lock.json') -PathType Leaf)
$publicCoreGiven = $PSBoundParameters.ContainsKey('PublicCore')
$version = $null
if ($repositoryMode) {
    $version = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'VERSION') -Raw).Trim()
    if ($PSCmdlet.ParameterSetName -eq 'Stage' -and -not $StageDirectory) {
        throw 'Run from the repository, this checks a package you name: -StageDirectory <folder> or -Zip <file>.'
    }
}
elseif ($PSCmdlet.ParameterSetName -eq 'Stage' -and -not $StageDirectory) {
    $StageDirectory = $PSScriptRoot
}

function Get-ExpectedPackageFiles {
    param([bool]$Core)
    if ($Core) {
        $files = @(
            'DLSSVideoPlayer.exe', 'nvngx_dlss.dll', 'nvngx_dlssg.dll', 'README.md', 'LICENSE',
            'SECURITY.md', 'CONTRIBUTING.md', 'CHANGELOG.md', 'THIRD_PARTY.md',
            'PUBLIC_RELEASE_NOTICE.txt', 'THIRD_PARTY_LICENSES/NVIDIA-DLSS-SDK.txt',
            'THIRD_PARTY_LICENSES/dlss5-feeder-MIT.txt',
            'THIRD_PARTY_LICENSES/nvidia-optical-flow-MIT.txt',
            'THIRD_PARTY_LICENSES/nvidia-video-codec-MIT.txt',
            'THIRD_PARTY_LICENSES/tabler-MIT.txt', 'docs/ARCHITECTURE.md',
            'docs/BUILDING.md', 'docs/DLSS5_SETUP.md', 'docs/RELATED_PROJECTS.md',
            'docs/TROUBLESHOOTING.md', 'PACKAGE_MANIFEST.txt',
            # This script ships inside the package it checks. README tells a user
            # to verify what they downloaded; without the checker in the zip they
            # would have to clone the repository to do it.
            'verify_package.ps1'
        )
    }
    else {
        $files = @(
            'DLSSVideoPlayer.exe', 'neural-runtime/NeuralWorker.exe', 'neural-runtime/nvngx_dlss.dll', 'ffmpeg.exe', 'ffprobe.exe', 'yt-dlp.exe', 'deno.exe',
            'neural-runtime/dxgi.dll', 'neural-runtime/ReShade.ini', 'neural-runtime/ReShadePreset.ini', 'neural-runtime/renodx-dlss5.addon64',
            'nvngx_dlss.dll', 'nvngx_dlssg.dll',
            'neural-runtime/nvngx_dlssnr.dll', 'neural-runtime/sl.common.dll', 'neural-runtime/sl.dlss.dll',
            'neural-runtime/sl.dlss_g.dll', 'neural-runtime/sl.dlss_nr.dll', 'neural-runtime/sl.interposer.dll', 'neural-runtime/sl.nis.dll',
            'neural-runtime/sl.pcl.dll', 'neural-runtime/sl.reflex.dll', 'README.md', 'LICENSE', 'SECURITY.md',
            'CONTRIBUTING.md', 'CHANGELOG.md', 'THIRD_PARTY.md',
            'THIRD_PARTY_LICENSES/yt-dlp-2026.08.19.txt',
            'THIRD_PARTY_LICENSES/deno-2.9.5.txt', 'THIRD_PARTY_LICENSES/ffmpeg.txt',
            'THIRD_PARTY_LICENSES/ffmpeg-GPL-3.0.txt',
            'THIRD_PARTY_LICENSES/experimental-runtime.txt',
            'THIRD_PARTY_LICENSES/reshade-BSD-3-Clause.txt',
            'THIRD_PARTY_LICENSES/renodx-MIT.txt',
            'THIRD_PARTY_LICENSES/nvidia-streamline-MIT.txt',
            'THIRD_PARTY_LICENSES/NVIDIA-DLSS-SDK.txt',
            'THIRD_PARTY_LICENSES/dlss5-feeder-MIT.txt',
            'THIRD_PARTY_LICENSES/nvidia-optical-flow-MIT.txt',
            'THIRD_PARTY_LICENSES/nvidia-video-codec-MIT.txt',
            'THIRD_PARTY_LICENSES/tabler-MIT.txt', 'docs/ARCHITECTURE.md',
            'docs/BUILDING.md', 'docs/DLSS5_SETUP.md', 'docs/RELATED_PROJECTS.md',
            'docs/TROUBLESHOOTING.md',
            'EXPERIMENTAL_RUNTIME_NOTICE.txt', 'PACKAGE_MANIFEST.txt',
            # Ships inside the package it checks, for the same reason as above.
            'verify_package.ps1'
        )
    }
    return @($files + @('docs/USAGE.md', 'docs/EXAMPLE_VIDEOS.md'))
}

# Package mode has neither VERSION nor a -PublicCore the user would have to
# know to pass. The manifest the packager wrote says both: its first line
# carries the version, and each variant ships a notice the other does not.
function Read-PackageIdentity {
    param([string]$Root)
    $manifestPath = Join-Path $Root 'PACKAGE_MANIFEST.txt'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "No PACKAGE_MANIFEST.txt in '$Root'. Run this from the unpacked package folder, or pass -StageDirectory <that folder>."
    }
    $lines = @(Get-Content -LiteralPath $manifestPath)
    if ($lines.Count -lt 3 -or $lines[0] -notmatch '^ProductVersion=([0-9]+\.[0-9]+\.[0-9]+)$') {
        throw 'PACKAGE_MANIFEST.txt has an invalid header.'
    }
    $manifestVersion = $Matches[1]
    $paths = @($lines[2..($lines.Count - 1)] | ForEach-Object { ($_ -split '\|', 2)[0] })
    $isCore = $paths -ccontains 'PUBLIC_RELEASE_NOTICE.txt'
    $isComplete = $paths -ccontains 'EXPERIMENTAL_RUNTIME_NOTICE.txt'
    if ($isCore -eq $isComplete) {
        throw 'PACKAGE_MANIFEST.txt lists neither or both variant notices, so it describes no package this project builds.'
    }
    # A folder that still carries the packager's name has to agree with the
    # manifest. A renamed folder is the user's business and is not checked.
    $folder = Split-Path -Leaf ([IO.Path]::GetFullPath($Root).TrimEnd('\', '/'))
    if ($folder -match '^DLSSVideoPlayer-v([0-9]+\.[0-9]+\.[0-9]+)(-.*)?-win64$' -and $Matches[1] -cne $manifestVersion) {
        throw "Folder '$folder' names version $($Matches[1]), but PACKAGE_MANIFEST.txt says $manifestVersion."
    }
    return [pscustomobject]@{ Version = $manifestVersion; Core = $isCore }
}

# The allowlist depends on the variant, which package mode only knows once it
# has found the manifest - inside the zip, for -Zip - so it is chosen per root.
$expected = @()
$optionalVsrRuntime = 'nvngx_vsr.dll'
function Select-PackageVariant {
    param([string]$Root)
    if ($repositoryMode) {
        $script:expected = Get-ExpectedPackageFiles -Core ([bool]$PublicCore)
        return
    }
    $identity = Read-PackageIdentity -Root $Root
    if ($publicCoreGiven -and ([bool]$PublicCore -ne $identity.Core)) {
        throw "-PublicCore:`$$([bool]$PublicCore) disagrees with the package, which is the $(if ($identity.Core) { 'core' } else { 'complete' }) variant."
    }
    $script:version = $identity.Version
    $script:PublicCore = [switch]$identity.Core
    $script:expected = Get-ExpectedPackageFiles -Core $identity.Core
    Write-Host "Package mode: DLSS 5 Video Player $($identity.Version), $(if ($identity.Core) { 'core' } else { 'complete' }) package."
}

$temporaryRoot = $null
$maxArchiveEntries = 128
$maxArchiveEntryBytes = 512MB
$maxArchiveTotalBytes = 1GB
$maxCompressionRatio = 250.0

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

function Sort-PackagePathsOrdinal {
    param([string[]]$Paths)
    [string[]]$sorted = @($Paths)
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    return $sorted
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

function Assert-NoSensitiveText {
    param([string]$RelativePath, [string]$Text)
    $checks = @(
        @('Windows absolute path', '(?i)(?<![A-Za-z0-9+.-])[A-Z]:[\\/]'),
        @('UNC path', '(?<!:)\\\\[A-Za-z0-9._-]+[\\/]'),
        @('user home path', '(?i)(?<![:A-Za-z0-9])/(?:Users|home)/[^/\s]+/'),
        @('private key', '-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
        @('AWS access key', '\bAKIA[0-9A-Z]{16}\b'),
        @('GitHub token', '\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})\b'),
        @('bearer token', '(?i)\bBearer\s+[A-Za-z0-9._~+/=-]{20,}'),
        @('credential-bearing URL', '(?i)https?://[^\s/@:]+:[^\s/@]+@')
    )
    foreach ($check in $checks) {
        if ($Text -match $check[1]) { throw "$($check[0]) leakage is forbidden in packaged text: $RelativePath" }
    }
}

# .gitattributes pins packaged Markdown to LF and every other packaged text
# file to CRLF, none with a BOM, and package_release.ps1 normalises the text it
# takes from outside the repository to the same shape. Anything else means the
# manifest hash would not match another machine's.
function Assert-PinnedLineEndings {
    param([string]$RelativePath, [string]$Text)
    if ($Text.Length -gt 0 -and $Text[0] -eq [char]0xFEFF) { throw "UTF-8 BOM is forbidden in packaged text: $RelativePath" }
    if ($RelativePath.EndsWith('.md', [StringComparison]::OrdinalIgnoreCase)) {
        if ($Text.Contains("`r")) { throw "Packaged Markdown must use LF line endings: $RelativePath" }
    }
    elseif ($Text -cmatch '(?<!\r)\n|\r(?!\n)') { throw "Packaged text must use CRLF line endings: $RelativePath" }
}

# GPLv3 section 4 requires the licence text itself to travel with ffmpeg.exe and
# ffprobe.exe. The packaged copy is the LICENSE file from the pinned FFmpeg
# archive (tools/fetch_ffmpeg_helpers.ps1), whose SHA-256 is below; the
# repository pins it to CRLF like every packaged .txt, so it is compared with
# its line endings folded back to that file's LF. An edited, truncated or
# re-flowed text fails here in both modes.
$ffmpegLicenseLfSha256 = '8CEB4B9EE5ADEDDE47B31E975C1D90C73AD27B6B165A1DCD80C7C545EB65B903'
function Assert-FfmpegLicenseText {
    param([string]$Root)
    $relative = 'THIRD_PARTY_LICENSES/ffmpeg-GPL-3.0.txt'
    $bytes = [IO.File]::ReadAllBytes((Join-Path $Root $relative))
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $lf = $latin1.GetBytes($latin1.GetString($bytes).Replace("`r`n", "`n"))
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha256.ComputeHash($lf)).Replace('-', '') }
    finally { $sha256.Dispose() }
    if ($hash -cne $ffmpegLicenseLfSha256) {
        throw "$relative is not the GPL text shipped with the pinned FFmpeg build (LF SHA-256 $hash)."
    }
}

function Assert-ReleaseExecutableIdentity {
    param([string]$Root)
    $identity = (Get-Item -LiteralPath (Join-Path $Root 'DLSSVideoPlayer.exe')).VersionInfo
    $expectedIdentity = @{
        ProductName = 'DLSS 5 Video Player'
        FileVersion = "$version.0"
        ProductVersion = "$version.0"
        OriginalFilename = 'DLSSVideoPlayer.exe'
    }
    foreach ($name in $expectedIdentity.Keys) {
        if ([string]$identity.$name -cne $expectedIdentity[$name]) {
            throw "Packaged executable $name mismatch: expected '$($expectedIdentity[$name])', received '$($identity.$name)'."
        }
    }
}

function Assert-LockedFiles {
    param([string]$Root)
    # The optional RTX VSR feature DLL is NVIDIA's release build from the RTX Video
    # SDK. No pinned copy lives in the repository - the SDK is behind a developer
    # login - so what can be checked in both modes is NVIDIA's signature on it.
    $vsrPath = Join-Path $Root $optionalVsrRuntime
    if (Test-Path -LiteralPath $vsrPath -PathType Leaf) {
        $signature = Get-AuthenticodeSignature -LiteralPath $vsrPath
        $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
        if ([string]$signature.Status -cne 'Valid' -or $subject -notlike '*NVIDIA*') {
            throw "Packaged $optionalVsrRuntime is not validly NVIDIA-signed: status=$($signature.Status) signer=$subject"
        }
    }
    if (-not $repositoryMode) {
        # An unpacked package has no pinned SDK and no lock files to compare
        # against; the manifest check that follows holds every byte to what
        # the packager hashed. What can still be checked independently of the
        # manifest is that NVIDIA's own snippets carry NVIDIA's signature, in
        # both variants. Whether the manifest itself is the one CI built is
        # what the zip's checksum and attestation answer, not this script.
        foreach ($snippet in @('nvngx_dlss.dll', 'nvngx_dlssg.dll')) {
            $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $Root $snippet)
            $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
            if ([string]$signature.Status -cne 'Valid' -or $subject -notlike '*NVIDIA*') {
                throw "Packaged $snippet is not validly NVIDIA-signed: status=$($signature.Status) signer=$subject"
            }
        }
        return
    }
    if ($PublicCore) {
        # Both NGX snippets in the public core are the pinned SDK's own files -
        # the Super Resolution runtime and the Frame Generation snippet - and
        # NVIDIA signs both, so both are held to the SDK bytes and to a valid
        # NVIDIA signature. The core ships no FFmpeg, so the conversion pass
        # that uses the second one cannot run there; it is packaged because the
        # core's rule is provenance rather than feature reach, which is what
        # PUBLIC_RELEASE_NOTICE.txt tells the reader.
        foreach ($snippet in @('nvngx_dlss.dll', 'nvngx_dlssg.dll')) {
            $packagedPath = Join-Path $Root $snippet
            $sdkPath = Join-Path $repositoryRoot "external\DLSS\lib\Windows_x86_64\rel\$snippet"
            if (-not (Test-Path -LiteralPath $sdkPath -PathType Leaf)) {
                throw "Pinned official DLSS runtime is missing: $sdkPath"
            }
            $packaged = Get-Item -LiteralPath $packagedPath
            $sdk = Get-Item -LiteralPath $sdkPath
            if ($packaged.Length -ne $sdk.Length -or
                (Get-Sha256 -Path $packagedPath) -cne (Get-Sha256 -Path $sdkPath)) {
                throw "Public package $snippet does not match the pinned official SDK."
            }
            $signature = Get-AuthenticodeSignature -LiteralPath $packagedPath
            $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
            if ([string]$signature.Status -cne 'Valid' -or $subject -notlike '*NVIDIA*') {
                throw "Public package $snippet is not validly NVIDIA-signed: status=$($signature.Status) signer=$subject"
            }
        }
        return
    }

    # NGX resolves a feature snippet from the directory of the process that
    # creates the feature, and Frame Generation is created by the player, not by
    # the render helper - so nvngx_dlssg.dll ships at the package root only. It
    # is deliberately NOT a runtime-lock entry: the lock is the helper's set,
    # every entry is required to exist under neural-runtime/, and adding a file
    # the helper never loads would both retire every cached render (the lock
    # feeds runtimeDigest) and refuse neural rendering outright on any install
    # whose neural-runtime/ predates it. Measured: with the entry present and
    # the file staged only beside the player, the very first log line of a fresh
    # session read "Neural pre-render failed: The configured neural runtime is
    # incomplete." Both root snippets are checked against the pinned SDK bytes
    # here instead.
    foreach($snippet in @('nvngx_dlss.dll','nvngx_dlssg.dll')){
        $rootSnippet=Join-Path $Root $snippet
        $officialSnippet=Join-Path $repositoryRoot "external/DLSS/lib/Windows_x86_64/rel/$snippet"
        if((Get-Sha256 -Path $rootSnippet) -cne (Get-Sha256 -Path $officialSnippet)) {
            throw "Playback runtime $snippet does not match the pinned official SDK."
        }
    }
    $runtimeLock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\runtime-lock.json') -Raw | ConvertFrom-Json
    foreach ($entry in $runtimeLock.entries) {
        $path = Join-Path (Join-Path $Root 'neural-runtime') ([string]$entry.destination)
        $item = Get-Item -LiteralPath $path
        $hash = Get-Sha256 -Path $path
        if ($item.Length -ne [int64]$entry.size -or $hash -cne [string]$entry.sha256) {
            throw "Locked runtime mismatch: $($entry.destination)"
        }
    }
    $toolLock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\tool-lock.json') -Raw | ConvertFrom-Json
    foreach ($entry in $toolLock.entries) {
        $path = Join-Path $Root ([string]$entry.name)
        $item = Get-Item -LiteralPath $path
        $hash = Get-Sha256 -Path $path
        if ($item.Length -ne [int64]$entry.size -or $hash -cne [string]$entry.sha256) {
            throw "Locked helper mismatch: $($entry.name)"
        }
    }
}

function Assert-Stage {
    param([string]$Root)
    $resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
    Select-PackageVariant -Root $resolvedRoot
    $actualUnsorted = @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File | ForEach-Object {
        Get-RelativePackagePath -Root $resolvedRoot -Path $_.FullName
    })
    $actual = @(Sort-PackagePathsOrdinal -Paths $actualUnsorted)
    # RTX VSR's feature DLL ships only from a build made with the RTX Video SDK
    # (docs/BUILDING.md), so it is allowed at the root and never required.
    $allowed = @($expected)
    if ($actual -ccontains $optionalVsrRuntime) { $allowed += $optionalVsrRuntime }
    $expectedSorted = @(Sort-PackagePathsOrdinal -Paths $allowed)
    if ([string]::Join("`n", $actual) -cne [string]::Join("`n", $expectedSorted)) {
        $missing = @($expectedSorted | Where-Object { $_ -cnotin $actual })
        $unexpected = @($actual | Where-Object { $_ -cnotin $expectedSorted })
        $message = "Package allowlist mismatch. Missing=[$([string]::Join(', ', $missing))] Unexpected=[$([string]::Join(', ', $unexpected))]"
        # The player writes its settings, log and caches beside itself, so a
        # folder it has already run from is expected to fail here.
        if (-not $repositoryMode -and $unexpected.Count -ne 0) {
            $message += ' Check a freshly unpacked copy: once the player has run, the files it writes beside itself are extra files too.'
        }
        throw $message
    }

    $knownExecutables = if ($PublicCore) { @('DLSSVideoPlayer.exe') } else { @('DLSSVideoPlayer.exe', 'neural-runtime/NeuralWorker.exe', 'ffmpeg.exe', 'ffprobe.exe', 'yt-dlp.exe', 'deno.exe') }
    $unexpectedExecutables = @($actual | Where-Object { $_.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase) -and $_ -cnotin $knownExecutables })
    if ($unexpectedExecutables.Count -ne 0) {
        throw "Unexpected launchable executable(s): $([string]::Join(', ', $unexpectedExecutables))"
    }

    if (-not $PublicCore) {
        foreach ($configName in @('neural-runtime/ReShade.ini', 'neural-runtime/ReShadePreset.ini')) {
            if ((Get-Item -LiteralPath (Join-Path $resolvedRoot $configName)).Length -eq 0) {
                throw "$configName must not be empty."
            }
        }

        $inOverlaySection = $false
        $tutorialProgress = $null
        foreach ($line in Get-Content -LiteralPath (Join-Path $resolvedRoot 'neural-runtime/ReShade.ini')) {
            $trimmed = $line.Trim()
            if ($trimmed -match '^\[(.+)\]$') {
                $inOverlaySection = $Matches[1] -ieq 'OVERLAY'
            }
            elseif ($inOverlaySection -and $trimmed -match '^TutorialProgress\s*=\s*(\d+)\s*$') {
                $tutorialProgress = $Matches[1]
            }
        }
        if ($tutorialProgress -cne '4') {
            throw 'ReShade.ini must ship with the ReShade tutorial already dismissed (OVERLAY/TutorialProgress=4).'
        }
    }

    Assert-ReleaseExecutableIdentity -Root $resolvedRoot

    foreach ($relative in $actual) {
        if ($relative -match '(?i)(^|[/_.-])pt-br([/_.-]|$)|languages/') {
            throw "Portuguese/language artifact is forbidden: $relative"
        }
        $path = Join-Path $resolvedRoot $relative
        if ($relative -match '(?i)(\.md|\.txt|\.ini)$' -or $relative -ceq 'LICENSE') {
            $text = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($path))
            Assert-PinnedLineEndings -RelativePath $relative -Text $text
            if ($text -match '(?i)\bpt-br\b|portugu[eê]s') {
                throw "Portuguese content marker is forbidden: $relative"
            }
            Assert-NoSensitiveText -RelativePath $relative -Text $text
        }
    }

    if (-not $PublicCore) { Assert-FfmpegLicenseText -Root $resolvedRoot }
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
    $signatures = New-Object Collections.Generic.List[string]
    for ($index = 0; $index -lt $manifestExpected.Count; ++$index) {
        $relative = $manifestExpected[$index]
        $path = Join-Path $resolvedRoot $relative
        $item = Get-Item -LiteralPath $path
        $hash = Get-Sha256 -Path $path
        $auth = Get-AuthenticodeRecord -Path $path
        $expectedLine = "$relative|$($item.Length)|$hash|$($auth.Status)|$($auth.Signer)"
        if ($manifestRows[$index] -cne $expectedLine) {
            throw "Manifest mismatch for '$relative'."
        }
        if ($auth.Status -cne 'N/A') {
            $signatures.Add("  $relative  $($auth.Status)$(if ($auth.Signer) { "  $($auth.Signer)" })")
        }
    }

    # What a user downloading this cannot see from a hash: which binaries are
    # signed, and by whom. The same state is recorded in the manifest and was
    # just held to it; this prints it.
    if (-not $repositoryMode) {
        Write-Host 'Authenticode state of the packaged binaries:'
        foreach ($line in $signatures) { Write-Host $line }
    }
    Write-Host "Verified allowlisted package with $($actual.Count) files at '$resolvedRoot'."
}

try {
    if ($PSCmdlet.ParameterSetName -eq 'Zip') {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zipPath = (Resolve-Path -LiteralPath $Zip).Path
        $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
        try {
            $entries = @($archive.Entries)
            if ($entries.Count -gt $maxArchiveEntries) {
                throw "ZIP contains $($entries.Count) entries; maximum is $maxArchiveEntries."
            }
            [uint64]$totalUncompressed = 0
            foreach ($entry in $entries) {
                [uint64]$length = $entry.Length
                [uint64]$compressedLength = $entry.CompressedLength
                if ($length -gt $maxArchiveEntryBytes) {
                    throw "ZIP entry exceeds the per-file limit: $($entry.FullName)"
                }
                if ($totalUncompressed -gt ([uint64]$maxArchiveTotalBytes - $length)) {
                    throw 'ZIP exceeds the total uncompressed-size limit.'
                }
                $totalUncompressed += $length
                if ($length -ge 1MB) {
                    if ($compressedLength -eq 0 -or ($length / [double]$compressedLength) -gt $maxCompressionRatio) {
                        throw "ZIP entry exceeds the compression-ratio limit: $($entry.FullName)"
                    }
                }
            }

            $entryNames = @($entries | Where-Object { $_.Name } | Select-Object -ExpandProperty FullName)
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
        # Outside the repository the version and variant come from the
        # manifest inside, and Read-PackageIdentity holds the folder name to it.
        if ($repositoryMode) {
            $expectedRootName = if ($PublicCore) { "DLSSVideoPlayer-v$version-core-win64" } else { "DLSSVideoPlayer-v$version$PackageSuffix-win64" }
            if ($roots[0].Name -cne $expectedRootName) {
                throw "ZIP release directory mismatch: expected '$expectedRootName', received '$($roots[0].Name)'."
            }
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
