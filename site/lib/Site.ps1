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

function Format-StarCount {
    <#  A star count as a person reads it: 127, 1,234, then 12.3k. $null or a
        non-number yields '' - the caller renders nothing rather than a zero,
        because an API that did not answer is not a repository nobody starred. #>
    param([AllowNull()]$Count)
    if ($null -eq $Count) { return '' }
    $n = 0L
    if (-not [long]::TryParse([string]$Count, [ref]$n) -or $n -lt 0) { return '' }
    $inv = [cultureinfo]::InvariantCulture
    if ($n -ge 10000) { return ([math]::Floor($n / 100) / 10).ToString('0.0', $inv) + 'k' }
    return $n.ToString('#,0', $inv)
}

function Get-StarSummary {
    <#  The GitHub star count in the three shapes the page uses, fetched at build
        time only: the page never asks the API itself, so the number is the one
        from when it was built, and says so in its title.

        With no count every shape is empty or falls back to the licence, so a
        build whose API call failed still renders a complete, honest link. #>
    param([AllowNull()]$Count, [string]$AsOf = '')

    $text = Format-StarCount $Count
    if (-not $text) {
        return [pscustomobject]@{ Text = ''; Badge = ''; Meta = 'MIT licence' }
    }
    $noun = if ($text -eq '1') { 'star' } else { 'stars' }
    $title = if ($AsOf) { "GitHub stars when this page was built, $AsOf" } else { 'GitHub stars when this page was built' }
    [pscustomobject]@{
        Text  = "$text $noun"
        Badge = "<span class=`"gh__stars`" title=`"$title`">$text<span class=`"gh__stars-noun`"> $noun</span></span>"
        Meta  = "$text $noun"
    }
}

function Get-MediaSize {
    <#  The pixel size a media file declares in its own header: PNG, JPEG, WebP,
        AVIF or MP4. Returns [pscustomobject]@{ Width; Height } or $null.

        Read from the bytes rather than through System.Drawing, because CI runs
        these tests under pwsh on Linux, and because AVIF, WebP and MP4 are not
        formats System.Drawing reads at all. Only the header is parsed; nothing
        is decoded.
    #>
    param([Parameter(Mandatory)][string]$Path)

    $b = [System.IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 32) { return $null }
    # Every byte is widened before it is shifted: -shl on a [byte] stays a
    # [byte] in PowerShell and silently drops the high bits.
    $u = { param($i) [long]$b[$i] }
    $be16 = { param($i) ((& $u $i) -shl 8) -bor (& $u ($i + 1)) }
    $be32 = { param($i) ((& $u $i) -shl 24) -bor ((& $u ($i + 1)) -shl 16) -bor ((& $u ($i + 2)) -shl 8) -bor (& $u ($i + 3)) }
    $le16 = { param($i) (& $u $i) -bor ((& $u ($i + 1)) -shl 8) }
    $le24 = { param($i) (& $u $i) -bor ((& $u ($i + 1)) -shl 8) -bor ((& $u ($i + 2)) -shl 16) }
    $size = { param($w, $h) [pscustomobject]@{ Width = [int]$w; Height = [int]$h } }
    $ascii = { param($i, $n) [System.Text.Encoding]::ASCII.GetString($b, $i, $n) }

    # PNG: the IHDR chunk is always first.
    if ($b[0] -eq 0x89 -and (& $ascii 1 3) -eq 'PNG') { return & $size (& $be32 16) (& $be32 20) }

    # JPEG: walk the markers to the first start-of-frame.
    if ($b[0] -eq 0xFF -and $b[1] -eq 0xD8) {
        $i = 2
        while ($i + 9 -lt $b.Length) {
            if ($b[$i] -ne 0xFF) { $i++; continue }
            $marker = $b[$i + 1]
            if ($marker -eq 0xD8 -or $marker -eq 0x01 -or ($marker -ge 0xD0 -and $marker -le 0xD7)) { $i += 2; continue }
            if ($marker -ge 0xC0 -and $marker -le 0xCF -and $marker -notin 0xC4, 0xC8, 0xCC) {
                return & $size (& $be16 ($i + 7)) (& $be16 ($i + 5))
            }
            $i += 2 + (& $be16 ($i + 2))
        }
        return $null
    }

    # WebP: lossy (VP8), lossless (VP8L) or extended (VP8X).
    if ((& $ascii 0 4) -eq 'RIFF' -and (& $ascii 8 4) -eq 'WEBP') {
        switch (& $ascii 12 4) {
            'VP8 ' { return & $size ((& $le16 26) -band 0x3FFF) ((& $le16 28) -band 0x3FFF) }
            'VP8L' {
                $bits = [long]$b[21] -bor ([long]$b[22] -shl 8) -bor ([long]$b[23] -shl 16) -bor ([long]$b[24] -shl 24)
                return & $size (($bits -band 0x3FFF) + 1) ((($bits -shr 14) -band 0x3FFF) + 1)
            }
            'VP8X' { return & $size ((& $le24 24) + 1) ((& $le24 27) + 1) }
        }
        return $null
    }

    # AVIF (the 'ispe' property) and MP4 (the video track's 'tkhd', 16.16
    # fixed point). Both are ISO boxes; a scan for the box type is enough for
    # files this project writes, which carry one image or one video track.
    if ((& $ascii 4 4) -eq 'ftyp') {
        $brand = & $ascii 8 4
        $text = [System.Text.Encoding]::ASCII.GetString($b)
        if ($brand -match '^avi[fs]$') {
            $at = $text.IndexOf('ispe')
            if ($at -ge 0) { return & $size (& $be32 ($at + 8)) (& $be32 ($at + 12)) }
            return $null
        }
        $at = 0
        while (($at = $text.IndexOf('tkhd', $at)) -ge 0) {
            $version = $b[$at + 4]
            # Width follows version/flags, the times and ids (20 or 32 bytes),
            # 16 bytes of layer/volume/reserved and the 36-byte matrix.
            $end = $at + $(if ($version -eq 1) { 92 } else { 80 })
            $w = (& $be32 $end) -shr 16
            $h = (& $be32 ($end + 4)) -shr 16
            if ($w -gt 0 -and $h -gt 0) { return & $size $w $h }
            $at += 4
        }
    }
    return $null
}

function Resolve-UnreleasedMarks {
    <#  Keeps or removes the page's "in main, after the last release" framing.

        The page shows features that are in main but not in the release its
        download button offers, marked New, with a note saying the next release
        carries them. Once a release newer than $LastWithout is what the page
        offers, that framing is false, so the build removes it: every
        <!-- unreleased -->...<!-- /unreleased --> block and every New mark.
        Otherwise it keeps the text and drops only the comment fences. With no
        resolvable release it keeps the framing, the cautious reading.
    #>
    param(
        [Parameter(Mandatory)][string]$Html,
        [AllowNull()][AllowEmptyString()][string]$ReleaseVersion,
        [Parameter(Mandatory)][string]$LastWithout
    )
    $parsed = $null
    $carried = [version]::TryParse([string]$ReleaseVersion, [ref]$parsed) -and $parsed -gt [version]$LastWithout
    $options = [System.Text.RegularExpressions.RegexOptions]::Singleline
    if ($carried) {
        $Html = [regex]::Replace($Html, '\s*<!-- unreleased -->.*?<!-- /unreleased -->', '', $options)
        return [regex]::Replace($Html, ' ?<span class="new"[^>]*>New</span>', '')
    }
    return $Html.Replace('<!-- unreleased -->', '').Replace('<!-- /unreleased -->', '')
}

function Get-VideoDuration {
    <#  An MP4's duration in seconds, from its 'mvhd' box, or $null. The page's
        structured data and sitemap state the demonstration's length; this is
        what they are checked against. #>
    param([Parameter(Mandatory)][string]$Path)
    $b = [System.IO.File]::ReadAllBytes($Path)
    $at = [System.Text.Encoding]::ASCII.GetString($b).IndexOf('mvhd')
    if ($at -lt 0) { return $null }
    $u32 = { param($i) ([long]$b[$i] -shl 24) -bor ([long]$b[$i + 1] -shl 16) -bor ([long]$b[$i + 2] -shl 8) -bor [long]$b[$i + 3] }
    if ($b[$at + 4] -eq 1) {
        $scale = & $u32 ($at + 24)
        $duration = ((& $u32 ($at + 28)) -shl 32) -bor (& $u32 ($at + 32))
    } else {
        $scale = & $u32 ($at + 16)
        $duration = & $u32 ($at + 20)
    }
    if ($scale -le 0) { return $null }
    return [double]$duration / $scale
}

function ConvertTo-HtmlText {
    param([AllowNull()][string]$Text)
    if ($null -eq $Text) { return '' }
    return [System.Net.WebUtility]::HtmlEncode($Text)
}
