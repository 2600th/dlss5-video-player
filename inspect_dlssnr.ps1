param(
    [Parameter(Mandatory=$true)][string]$Path
)
$ErrorActionPreference = 'Stop'
try {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $fi = Get-Item -LiteralPath $resolved
    $ver = [Diagnostics.FileVersionInfo]::GetVersionInfo($resolved)
    $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $resolved
    $sig = Get-AuthenticodeSignature -LiteralPath $resolved

    Write-Host ('     File       : ' + $fi.Name)
    Write-Host ('     Size       : ' + $fi.Length + ' bytes')
    Write-Host ('     FileVersion: ' + $(if($ver.FileVersion){$ver.FileVersion}else{'<none>'}))
    Write-Host ('     ProductVer : ' + $(if($ver.ProductVersion){$ver.ProductVersion}else{'<none>'}))
    Write-Host ('     SHA256     : ' + $hash.Hash)
    Write-Host ('     Signature  : ' + $sig.Status)

    $srPath = Join-Path $fi.DirectoryName 'nvngx_dlss.dll'
    if (Test-Path -LiteralPath $srPath) {
        $srVer = [Diagnostics.FileVersionInfo]::GetVersionInfo($srPath)
        $srHash = Get-FileHash -Algorithm SHA256 -LiteralPath $srPath
        Write-Host ('     SR version : ' + $(if($srVer.FileVersion){$srVer.FileVersion}else{'<none>'}))
        Write-Host ('     SR SHA256  : ' + $srHash.Hash)
        if ($ver.FileVersion -and $srVer.FileVersion) {
            $nrCore = ($ver.FileVersion -split '\s')[0]
            $srCore = ($srVer.FileVersion -split '\s')[0]
            if ($nrCore -ne $srCore) {
                Write-Host ('[WARN] nvngx_dlss.dll and nvngx_dlssnr.dll versions differ (' + $srCore + ' vs ' + $nrCore + '). For the experimental NR pack, prefer the matching pair supplied together.')
            } else {
                Write-Host ('[OK] DLSS SR/NR file versions match: ' + $nrCore)
            }
        }
    }
    if ($sig.SignerCertificate) {
        Write-Host ('     Signer     : ' + $sig.SignerCertificate.Subject)
        Write-Host ('     Cert thumb : ' + $sig.SignerCertificate.Thumbprint)
        Write-Host ('     Cert until : ' + $sig.SignerCertificate.NotAfter.ToString('u'))
    }
    if ($sig.TimeStamperCertificate) {
        Write-Host ('     Timestamp  : ' + $sig.TimeStamperCertificate.Subject)
    }

    switch ($sig.Status.ToString()) {
        'Valid'       { Write-Host '[OK] nvngx_dlssnr.dll Authenticode signature is valid.'; exit 0 }
        'NotSigned'   { Write-Host '[WARN] nvngx_dlssnr.dll is not Authenticode-signed. Experimental/repacked binaries may do this, but treat the file as unverified.'; exit 1 }
        'HashMismatch'{ Write-Host '[WARN] nvngx_dlssnr.dll signature hash does not match the signed image. The file was modified after signing or is corrupt.'; exit 2 }
        default       { Write-Host ('[WARN] nvngx_dlssnr.dll signature status: ' + $sig.Status + ' - ' + $sig.StatusMessage); exit 3 }
    }
} catch {
    Write-Host ('[WARN] Could not inspect nvngx_dlssnr.dll: ' + $_.Exception.Message)
    exit 4
}
