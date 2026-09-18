#Requires -Version 5.1
<#
    Pure helpers for the project website build.

    Everything here is deterministic and offline: it takes a release object that
    someone else fetched and turns it into the facts the page states. build.ps1
    owns the network and the filesystem; test.ps1 drives these functions from
    fixtures. Keeping the two apart is what lets the failure states - a release
    whose full package is not attached yet, no published release at all - be
    tested without a tag existing to produce them.
#>

Set-StrictMode -Version Latest

$script:DefaultRepo = '2600th/dlss5-video-player'
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Read-TextFile {
    <#  Reads a file as UTF-8, explicitly.

        Windows PowerShell's Get-Content decodes a file with no byte-order mark
        using the system ANSI codepage, which silently mangles every non-ASCII
        character in the templates - em dashes, multiplication signs, middle
        dots - and Set-Content then re-encodes the damage. This page is full of
        those characters, so its text I/O goes through here and Write-TextFile.
    #>
    param([Parameter(Mandatory)][string]$Path)
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

function Write-TextFile {
    <#  Writes UTF-8 with no byte-order mark, on every PowerShell version. #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, $script:Utf8NoBom)
}

function Get-RepoSlug {
    <#  The repository this site is built for. GITHUB_REPOSITORY is set in CI,
        so a fork builds a page pointing at the fork's own releases. #>
    if ($env:GITHUB_REPOSITORY) { return $env:GITHUB_REPOSITORY }
    return $script:DefaultRepo
}

function Format-ByteSize {
    param([Parameter(Mandatory)][long]$Bytes)
    if ($Bytes -ge 1GB) { return ('{0:0.0} GB' -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ('{0:0} MB'   -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ('{0:0} KB'   -f ($Bytes / 1KB)) }
    return "$Bytes B"
}

function Get-Property {
    # StrictMode makes a missing property fatal; fixtures and API payloads both
    # vary, so every read of untrusted shape goes through here.
    param($Object, [string]$Name, $Default = $null)
    if ($null -eq $Object) { return $Default }
    $prop = $Object.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $Default }
    if ($null -eq $prop.Value) { return $Default }
    return $prop.Value
}

function Select-PackageAsset {
    <#  Picks one package out of a release's assets.

        The two zips do not share a name stem - 'dlss5-video-player-...' for the
        complete package, 'DLSSVideoPlayer-...' for the core one - and their case
        differs, so matching is case-insensitive and stem-agnostic. Checksum
        sidecars are excluded first: `*-win64.zip.sha256` would otherwise match
        a pattern written for `*-win64.zip`.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Assets,
        [Parameter(Mandatory)][ValidateSet('Full', 'Core')][string]$Kind
    )

    $zips = @($Assets | Where-Object {
        $name = Get-Property $_ 'name' ''
        $name -like '*.zip' -and $name -notlike '*.sha256'
    })

    $match = switch ($Kind) {
        'Core' { $zips | Where-Object { (Get-Property $_ 'name' '') -like '*-core-win64.zip' } }
        'Full' { $zips | Where-Object {
                    $n = Get-Property $_ 'name' ''
                    $n -like '*-win64.zip' -and $n -notlike '*-core-win64.zip'
                 } }
    }

    $asset = @($match) | Select-Object -First 1
    if ($null -eq $asset) { return $null }

    $name = Get-Property $asset 'name' ''
    $url = Get-Property $asset 'browser_download_url' ''
    $sidecar = @($Assets | Where-Object { (Get-Property $_ 'name' '') -eq "$name.sha256" }) |
        Select-Object -First 1

    [pscustomobject]@{
        Name      = $name
        Url       = $url
        Size      = [long](Get-Property $asset 'size' 0)
        SizeText  = Format-ByteSize ([long](Get-Property $asset 'size' 0))
        Sha256    = $null   # filled in by build.ps1 when the sidecar is fetchable
        Sha256Url = if ($sidecar) { Get-Property $sidecar 'browser_download_url' '' } else { $null }
    }
}

function Resolve-ReleaseData {
    <#  Normalizes a GitHub release into the facts the page states.

        A release with no full package attached is a supported state, not an
        error: CI attaches only the core zip, and the 308 MB complete package is
        attached by hand before publishing. Passing $null - no published release
        at all - yields a record whose HasDownload is false, which the template
        renders as a link to the release list rather than a fabricated download.
    #>
    param($Release, [string]$Repo = (Get-RepoSlug))

    $releasesUrl = "https://github.com/$Repo/releases"

    if ($null -eq $Release -or ($Release -is [array] -and $Release.Count -eq 0)) {
        return [pscustomobject]@{
            Version     = $null
            Tag         = $null
            ReleaseUrl  = $releasesUrl
            PublishedAt = $null
            Full        = $null
            Core        = $null
            HasDownload = $false
        }
    }

    if ($Release -is [array]) { $Release = $Release[0] }

    $tag = Get-Property $Release 'tag_name' ''
    $version = $tag -replace '^dlss5-video-player-v', ''
    $assets = @(Get-Property $Release 'assets' @())

    $full = Select-PackageAsset -Assets $assets -Kind 'Full'
    $core = Select-PackageAsset -Assets $assets -Kind 'Core'

    [pscustomobject]@{
        Version     = $version
        Tag         = $tag
        ReleaseUrl  = Get-Property $Release 'html_url' "$releasesUrl/tag/$tag"
        PublishedAt = Get-Property $Release 'published_at' $null
        Full        = $full
        Core        = $core
        HasDownload = ($null -ne $full -or $null -ne $core)
    }
}

function Get-AnalyticsSnippet {
    <#  Returns the analytics markup for a measurement id, or a comment.

        Unset means silent: forks and local previews must not report to the
        maintainer's property. A malformed id throws rather than degrading,
        because a typo that silently collects nothing is discovered months later
        as an empty dashboard.
    #>
    param([AllowNull()][string]$MeasurementId, [Parameter(Mandatory)][string]$PartialPath)

    if ([string]::IsNullOrWhiteSpace($MeasurementId)) {
        return '<!-- analytics: GA_MEASUREMENT_ID not set for this build -->'
    }

    $id = $MeasurementId.Trim()
    if ($id -notmatch '^G-[A-Z0-9]{6,}$') {
        throw "GA_MEASUREMENT_ID '$id' is not a GA4 measurement id (expected G-XXXXXXXXXX). Refusing to build a page whose analytics would silently collect nothing."
    }

    $partial = Read-TextFile -Path $PartialPath
    return $partial.Replace('{{GA_MEASUREMENT_ID}}', $id).TrimEnd()
}

function Expand-Token {
    <#  Substitutes {{TOKEN}} placeholders and refuses to leave one behind. #>
    param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][hashtable]$Values)

    $result = $Text
    foreach ($key in $Values.Keys) {
        $result = $result.Replace("{{$key}}", [string]$Values[$key])
    }

    # @() matters: with no matches this pipeline yields $null, and Set-StrictMode
    # makes reading .Count on $null a terminating error in Windows PowerShell.
    $leftover = @([regex]::Matches($result, '\{\{([A-Z0-9_]+)\}\}') |
        ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    if ($leftover.Count -gt 0) {
        throw "Template token(s) never substituted: $($leftover -join ', ')"
    }
    return $result
}

function Get-ChecksumFromResponse {
    <#  Reads the hash out of a .sha256 sidecar's body.

        GitHub serves these as application/octet-stream, and Windows PowerShell
        hands back a byte array rather than a string for that content type - so
        matching the hash directly against the response body quietly finds
        nothing and the page falls back to linking the sidecar. Decode first.
    #>
    param($Content)

    if ($null -eq $Content) { return $null }
    $text = if ($Content -is [byte[]]) { [System.Text.Encoding]::UTF8.GetString($Content) } else { [string]$Content }
    if ($text -match '([0-9a-fA-F]{64})') { return $Matches[1].ToLowerInvariant() }
    return $null
}

function ConvertTo-HtmlText {
    param([AllowNull()][string]$Text)
    if ($null -eq $Text) { return '' }
    return [System.Net.WebUtility]::HtmlEncode($Text)
}
