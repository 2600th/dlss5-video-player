[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ytDlpVersion = '2026.08.19'
$ytDlpUrl = 'https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe'
$ytDlpSha256 = '66674953FE251B89F4D08C5F0E35E0728679BD67AB3D7D05C0562AF101DD3E7A'
$denoVersion = '2.9.5'
$denoUrl = 'https://github.com/denoland/deno/releases/download/v2.9.5/deno-x86_64-pc-windows-msvc.zip'
$denoSha256 = '171EFAB55AC6B9881FD53EE4C20F8BF3BB1340FFC618483746909014DB12216A'
$downloadTimeoutSeconds = 120

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repositoryRoot 'external\youtube'
$destinationParent = Split-Path -Parent $destination
$transactionId = [Guid]::NewGuid().ToString('N')
$stagedDestination = Join-Path $destinationParent ('.youtube-stage-{0}' -f $transactionId)
$backupDestination = Join-Path $destinationParent ('.youtube-backup-{0}' -f $transactionId)
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('dlss-player-youtube-{0}' -f [Guid]::NewGuid().ToString('N'))
$ytDlpDownload = Join-Path $temporaryRoot 'yt-dlp.exe'
$denoArchive = Join-Path $temporaryRoot 'deno.zip'
$denoExtracted = Join-Path $temporaryRoot 'deno'
$destinationBackedUp = $false
$transactionState = 'Precommit'

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

function Assert-Sha256 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actual -cne $Expected) {
        throw "Integrity check failed for $Name. Expected '$Expected' but received '$actual'."
    }
}

function Invoke-CheckedVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $output = @(& $Path --version)
    if ($LASTEXITCODE -ne 0) {
        throw "$Name --version failed with exit code $LASTEXITCODE."
    }
    if ($output.Count -eq 0) {
        throw "$Name --version returned no output."
    }
    return $output
}

function Invoke-BestEffortCleanup {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Description
    )

    try {
        # TEST-SEAM: before disposable cleanup
        Remove-ScopedDirectory -Path $Path -Parent $Parent
    }
    catch {
        Write-Warning ("Cleanup could not remove {0} '{1}': {2}" -f $Description, $Path, $_.Exception.Message)
    }
}

try {
    New-Item -ItemType Directory -Path $temporaryRoot, $denoExtracted -Force | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    Invoke-WebRequest -Uri $ytDlpUrl -OutFile $ytDlpDownload -UseBasicParsing -TimeoutSec $downloadTimeoutSeconds
    Invoke-WebRequest -Uri $denoUrl -OutFile $denoArchive -UseBasicParsing -TimeoutSec $downloadTimeoutSeconds

    Assert-Sha256 -Path $ytDlpDownload -Expected $ytDlpSha256 -Name "yt-dlp $ytDlpVersion"
    Assert-Sha256 -Path $denoArchive -Expected $denoSha256 -Name "Deno $denoVersion archive"

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($denoArchive, $denoExtracted)
    $denoExecutable = Join-Path $denoExtracted 'deno.exe'
    if (-not (Test-Path -LiteralPath $denoExecutable -PathType Leaf)) {
        throw "The verified Deno archive did not contain deno.exe at its root."
    }

    $ytOutput = Invoke-CheckedVersion -Path $ytDlpDownload -Name 'yt-dlp'
    $actualYtDlpVersion = ([string]::Join([Environment]::NewLine, $ytOutput)).Trim()
    if ($actualYtDlpVersion -cne $ytDlpVersion) {
        throw "Version check failed for yt-dlp. Expected '$ytDlpVersion' but received '$actualYtDlpVersion'."
    }

    $denoOutput = Invoke-CheckedVersion -Path $denoExecutable -Name 'Deno'
    $actualDenoVersionLine = ([string]$denoOutput[0]).Trim()
    $actualDenoVersion = ([regex]::Match($actualDenoVersionLine, '^deno\s+\S+')).Value
    $expectedDenoVersion = "deno $denoVersion"
    if ($actualDenoVersion -cne $expectedDenoVersion) {
        throw "Version check failed for Deno. Expected '$expectedDenoVersion' but received '$actualDenoVersionLine'."
    }

    New-Item -ItemType Directory -Path $destinationParent, $stagedDestination -Force | Out-Null
    if (Test-Path -LiteralPath $destination -PathType Container) {
        foreach ($item in Get-ChildItem -LiteralPath $destination -Force) {
            Copy-Item -LiteralPath $item.FullName -Destination $stagedDestination -Recurse -Force
        }
    }
    Copy-Item -LiteralPath $ytDlpDownload -Destination (Join-Path $stagedDestination 'yt-dlp.exe') -Force
    Copy-Item -LiteralPath $denoExecutable -Destination (Join-Path $stagedDestination 'deno.exe') -Force

    Assert-Sha256 -Path (Join-Path $stagedDestination 'yt-dlp.exe') -Expected $ytDlpSha256 -Name "staged yt-dlp $ytDlpVersion"
    $stagedYtOutput = Invoke-CheckedVersion -Path (Join-Path $stagedDestination 'yt-dlp.exe') -Name 'staged yt-dlp'
    if (([string]::Join([Environment]::NewLine, $stagedYtOutput)).Trim() -cne $ytDlpVersion) {
        throw 'The staged yt-dlp executable did not retain the verified version.'
    }
    $stagedDenoOutput = Invoke-CheckedVersion -Path (Join-Path $stagedDestination 'deno.exe') -Name 'staged Deno'
    $stagedDenoIdentity = ([regex]::Match(([string]$stagedDenoOutput[0]).Trim(), '^deno\s+\S+')).Value
    if ($stagedDenoIdentity -cne $expectedDenoVersion) {
        throw 'The staged Deno executable did not retain the verified version.'
    }

    try {
        if (Test-Path -LiteralPath $destination) {
            [IO.Directory]::Move($destination, $backupDestination)
            $destinationBackedUp = $true
            $transactionState = 'BackupCreated'
        }
        # TEST-SEAM: before staged destination commit
        [IO.Directory]::Move($stagedDestination, $destination)
        $transactionState = 'Committed'
    }
    catch {
        $swapFailure = $_
        if ($destinationBackedUp) {
            try {
                Remove-ScopedDirectory -Path $destination -Parent $destinationParent
                # TEST-SEAM: before backup restoration
                [IO.Directory]::Move($backupDestination, $destination)
                $destinationBackedUp = $false
                $transactionState = 'Restored'
            }
            catch {
                $restoreFailure = $_
                $transactionState = 'RestoreFailed'
                $message = "YouTube helper commit failed: $($swapFailure.Exception.Message) Restoration also failed: $($restoreFailure.Exception.Message) The previous destination backup is preserved at '$backupDestination'."
                throw (New-Object InvalidOperationException -ArgumentList $message, $restoreFailure.Exception)
            }
        }
        throw $swapFailure
    }

    if ($destinationBackedUp) {
        try {
            # TEST-SEAM: before committed backup cleanup
            Remove-ScopedDirectory -Path $backupDestination -Parent $destinationParent
            $destinationBackedUp = Test-Path -LiteralPath $backupDestination
        }
        catch {
            Write-Warning ("YouTube helpers were committed successfully, but the previous destination backup could not be removed and remains at '{0}': {1}" -f $backupDestination, $_.Exception.Message)
        }
    }

    Write-Host "Verified and staged yt-dlp $ytDlpVersion and Deno $denoVersion."
}
finally {
    Invoke-BestEffortCleanup -Path $temporaryRoot -Parent ([IO.Path]::GetTempPath()) -Description 'download staging'
    Invoke-BestEffortCleanup -Path $stagedDestination -Parent $destinationParent -Description 'destination staging'

    if ($transactionState -eq 'RestoreFailed' -and (Test-Path -LiteralPath $backupDestination)) {
        Write-Warning "Restoration failed; the previous destination backup is intentionally preserved at '$backupDestination'."
    }
    elseif ($transactionState -eq 'Committed' -and $destinationBackedUp -and (Test-Path -LiteralPath $backupDestination)) {
        Write-Warning "The new YouTube helper pair is committed and usable; cleanup is incomplete because the previous destination backup remains at '$backupDestination'."
    }
}
