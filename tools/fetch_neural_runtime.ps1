[CmdletBinding()]
param([string]$DestinationDirectory = '')

# Downloads every file in packaging/runtime-lock.json from its public source,
# verifies the archive digest, extracts the locked members, verifies each one
# against the lock (size + SHA-256), and stages them into external/runtime
# under the lock's `sourceName`. A member that is already staged and matches
# the lock is not downloaded again. Full validation (Authenticode state,
# numeric file version) is then run by stage_runtime.ps1.
#
# Fetching for a local build is the user obtaining the files from their
# upstream publishers. It does not change the redistribution status of the
# combined runtime set; see THIRD_PARTY.md and EXPERIMENTAL_RUNTIME_NOTICE.txt.

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProgressPreference = 'SilentlyContinue'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $DestinationDirectory) {
    $DestinationDirectory = Join-Path $repositoryRoot 'external\runtime'
}
$lock = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\runtime-lock.json') -Raw | ConvertFrom-Json

# One row per archive: where it lives, what it must hash to, and which archive
# member provides which lock destination.
$sources = @(
    @{
        Name = 'nvngx_dlssnr_310.8.SF-v2.zip'
        Url = 'https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.SF-v2/nvngx_dlssnr_310.8.SF-v2.zip'
        Sha256 = '1DA35941894994EB087E017577829E492454E9BAE3A6A9397027069CEB74955C'
        Members = @{ 'nvngx_dlssnr.dll' = 'nvngx_dlssnr.dll' }
    },
    @{
        Name = 'nvngx_dlss_310.8.0.zip'
        Url = 'https://github.com/RankFTW/rhi-repo/releases/download/dlss-310.8.0/nvngx_dlss_310.8.0.zip'
        Sha256 = 'FB481660F7E952B87F91760E3AFD7F9DC14CD2C3361B470E948D6346E4323009'
        Members = @{ 'nvngx_dlss.dll' = 'nvngx_dlss.dll' }
    },
    @{
        Name = 'renodx-dlss5_4.70.zip'
        Url = 'https://github.com/RankFTW/rhi-repo/releases/download/renodx-dlss5-4.70/renodx-dlss5_4.70.zip'
        Sha256 = 'D6E356D01B429AF6288F488A4926C44F1D779A7D4586EE8C79D04D3A09A536E6'
        Members = @{ 'renodx-dlss5.addon64' = 'renodx-dlss5.addon64' }
    },
    @{
        Name = 'streamline_2.13.0.0.zip'
        Url = 'https://github.com/RankFTW/rhi-repo/releases/download/streamline-2.13.0.0/streamline_2.13.0.0.zip'
        Sha256 = '7F38B5B2B7CC83AE859CCB3266160B8E5E517F379F81987FE3C758AE5C9E8C02'
        Members = @{
            'sl.common.dll' = 'sl.common.dll'; 'sl.dlss.dll' = 'sl.dlss.dll'; 'sl.dlss_g.dll' = 'sl.dlss_g.dll'
            'sl.dlss_nr.dll' = 'sl.dlss_nr.dll'; 'sl.interposer.dll' = 'sl.interposer.dll'; 'sl.nis.dll' = 'sl.nis.dll'
            'sl.pcl.dll' = 'sl.pcl.dll'; 'sl.reflex.dll' = 'sl.reflex.dll'
        }
    },
    @{
        # The ReShade installer is a ZIP container carrying the proxy DLLs. Its
        # central directory count does not match its end-of-central-directory
        # record, which .NET's ZipFile refuses; libarchive's tar reads it.
        Name = 'ReShade_Setup_6.8.0_Addon.exe'
        Url = 'https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe'
        Sha256 = 'AFE4C8F13048306307983B8B3D41D5BF00A86820440B0E57DEA10950E1176445'
        Members = @{ 'ReShade64.dll' = 'dxgi.dll' }
        Extractor = 'tar'
    }
)

$entriesByDestination = @{}
foreach ($entry in $lock.entries) { $entriesByDestination[[string]$entry.destination] = $entry }
$covered = @($sources | ForEach-Object { $_.Members.Values }) | Sort-Object -Unique
$locked = @($lock.entries | ForEach-Object { [string]$_.destination }) | Sort-Object -Unique
if (Compare-Object $covered $locked) {
    throw "fetch_neural_runtime.ps1 sources cover [$($covered -join ', ')] but the lock names [$($locked -join ', ')]."
}

function Test-LockedFile {
    param([Parameter(Mandatory = $true)]$Entry, [Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $item = Get-Item -LiteralPath $Path
    return ($item.Length -eq [int64]$Entry.size) -and
           ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ceq [string]$Entry.sha256)
}

function Get-StagedPath {
    param([Parameter(Mandatory = $true)]$Entry)
    return Join-Path $DestinationDirectory ([string]$Entry.sourceName)
}

# Extracts one archive member to $OutputPath with .NET's ZipFile or, for a
# container .NET cannot parse, the bsdtar Windows ships in System32.
function Expand-ArchiveMember {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$MemberName,
        [Parameter(Mandatory = $true)][string]$OutputPath,
        [Parameter(Mandatory = $true)][string]$Extractor
    )
    if ($Extractor -eq 'tar') {
        $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
        if (-not (Test-Path -LiteralPath $tar)) { throw "bsdtar is required to open '$ArchivePath' and was not found at '$tar'." }
        $scratch = Join-Path (Split-Path -Parent $OutputPath) ('extract-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $scratch | Out-Null
        & $tar -xf $ArchivePath -C $scratch $MemberName
        if ($LASTEXITCODE -ne 0) { throw "bsdtar could not extract $MemberName from '$ArchivePath' (exit $LASTEXITCODE)." }
        Move-Item -LiteralPath (Join-Path $scratch $MemberName) -Destination $OutputPath -Force
        Remove-Item -LiteralPath $scratch -Recurse -Force
        return
    }
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $zipEntry = $archive.GetEntry($MemberName)
        if (-not $zipEntry) { throw "'$ArchivePath' is missing $MemberName." }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($zipEntry, $OutputPath, $true)
    }
    finally { $archive.Dispose() }
}

New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('dlss-player-runtime-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $fetched = 0
    $kept = 0
    foreach ($source in $sources) {
        $missing = @($source.Members.GetEnumerator() | Where-Object {
            -not (Test-LockedFile -Entry $entriesByDestination[$_.Value] -Path (Get-StagedPath -Entry $entriesByDestination[$_.Value]))
        })
        if ($missing.Count -eq 0) {
            $kept += $source.Members.Count
            continue
        }

        $archivePath = Join-Path $temporaryRoot $source.Name
        Write-Host "Fetching $($source.Name) ..."
        Invoke-WebRequest -Uri $source.Url -OutFile $archivePath -UseBasicParsing -TimeoutSec 900 `
            -UserAgent 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) dlss5-video-player/fetch_neural_runtime'
        if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -cne $source.Sha256) {
            throw "Archive SHA-256 mismatch for $($source.Name); the upstream asset changed or the download is corrupt."
        }

        # Verify every member of this archive before staging any of them.
        $extractor = if ($source.ContainsKey('Extractor')) { $source.Extractor } else { 'zip' }
        $staged = @()
        foreach ($member in $missing) {
            $entry = $entriesByDestination[$member.Value]
            $extracted = Join-Path $temporaryRoot ([string]$entry.sourceName)
            Expand-ArchiveMember -ArchivePath $archivePath -MemberName $member.Key -OutputPath $extracted -Extractor $extractor
            if (-not (Test-LockedFile -Entry $entry -Path $extracted)) {
                throw "Runtime lock mismatch for $($member.Key) from $($source.Name)."
            }
            $staged += @{ From = $extracted; To = (Get-StagedPath -Entry $entry) }
        }
        foreach ($file in $staged) { Copy-Item -LiteralPath $file.From -Destination $file.To -Force }
        $fetched += $staged.Count
    }

    Write-Host "Neural runtime: $fetched file(s) fetched, $kept already matched the lock, in '$DestinationDirectory'."
    & (Join-Path $PSScriptRoot 'stage_runtime.ps1') -InputDirectory $DestinationDirectory -Destination $DestinationDirectory -ValidateOnly
}
finally {
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $temporaryFull = [IO.Path]::GetFullPath($temporaryRoot)
    if (-not $temporaryFull.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe temporary cleanup path.'
    }
    if (Test-Path -LiteralPath $temporaryFull) { Remove-Item -LiteralPath $temporaryFull -Recurse -Force }
}
