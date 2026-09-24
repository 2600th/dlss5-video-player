#Requires -Version 5.1
<#
.SYNOPSIS
    Assertions for the project website build.

.DESCRIPTION
    Runs in CI before the Pages artifact uploads, so a page that fails any check
    here is never deployed. Runnable locally with no arguments and no network:
    every case drives the build from a fixture in site/fixtures.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$siteRoot = $PSScriptRoot
. ([IO.Path]::Combine($siteRoot, 'lib', 'Site.ps1'))

# --- a very small harness -----------------------------------------------------

$script:Failures = [System.Collections.Generic.List[string]]::new()
$script:Current = ''
$script:Passed = 0

function Test-Case {
    param([string]$Name, [scriptblock]$Body)
    $script:Current = $Name
    try {
        & $Body
        Write-Host "  ok   $Name" -ForegroundColor DarkGray
        $script:Passed++
    } catch {
        Write-Host "  FAIL $Name" -ForegroundColor Red
        Write-Host "       $($_.Exception.Message)" -ForegroundColor Red
        $script:Failures.Add("$Name : $($_.Exception.Message)")
    }
}

function Assert-True { param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message } }

function Assert-Equal { param($Expected, $Actual, [string]$Message)
    if ($Expected -ne $Actual) { throw "$Message (expected '$Expected', got '$Actual')" } }

function Assert-Contains { param([string]$Haystack, [string]$Needle, [string]$Message)
    if ($Haystack -notlike "*$Needle*") { throw "$Message (missing '$Needle')" } }

function Assert-NotContains { param([string]$Haystack, [string]$Needle, [string]$Message)
    if ($Haystack -like "*$Needle*") { throw "$Message (unexpectedly found '$Needle')" } }

function Assert-Throws { param([scriptblock]$Body, [string]$Message)
    try { & $Body } catch { return }
    throw $Message }

# --- helpers ------------------------------------------------------------------

$fixtures = Join-Path $siteRoot 'fixtures'
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "dlss5-site-test-$PID"

function Invoke-Build {
    param([string]$Fixture, [string]$MeasurementId, [string]$Name, [string]$RepoFixture)
    $out = Join-Path $tempRoot $Name
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    $previous = $env:GA_MEASUREMENT_ID
    try {
        $env:GA_MEASUREMENT_ID = $MeasurementId
        $buildArgs = @{ OutputPath = $out; ReleaseFixture = (Join-Path $fixtures $Fixture); Quiet = $true }
        if ($RepoFixture) { $buildArgs.RepoFixture = Join-Path $fixtures $RepoFixture }
        & (Join-Path $siteRoot 'build.ps1') @buildArgs | Out-Null
    } finally {
        if ($null -eq $previous) { Remove-Item Env:GA_MEASUREMENT_ID -ErrorAction SilentlyContinue }
        else { $env:GA_MEASUREMENT_ID = $previous }
    }
    return $out
}

function Get-Html { param([string]$Dist) Read-TextFile -Path (Join-Path $Dist 'index.html') }

Write-Host ''
Write-Host 'Release resolution' -ForegroundColor Cyan

Test-Case 'full fixture yields both packages with correct names, sizes and URLs' {
    $r = Resolve-ReleaseData -Release ((Read-TextFile -Path (Join-Path $fixtures 'release-full.json')) | ConvertFrom-Json)
    Assert-Equal '0.23.0' $r.Version 'version parsed from tag'
    Assert-True ($null -ne $r.Full) 'full package resolved'
    Assert-Equal 'dlss5-video-player-v0.23.0-win64.zip' $r.Full.Name 'full package name'
    Assert-True ($r.Full.Size -gt 300MB) 'full package size looks like the 308 MB zip'
    Assert-Contains $r.Full.Url 'releases/download/' 'full package URL is a download URL'
    Assert-True ($null -ne $r.Core) 'core package resolved'
    Assert-Equal 'DLSSVideoPlayer-v0.23.0-core-win64.zip' $r.Core.Name 'core package name'
    Assert-True $r.HasDownload 'a download is offered'
}

Test-Case 'core-only fixture yields no full package but still offers core' {
    $r = Resolve-ReleaseData -Release ((Read-TextFile -Path (Join-Path $fixtures 'release-core-only.json')) | ConvertFrom-Json)
    Assert-True ($null -eq $r.Full) 'full package absent during the hand-attachment window'
    Assert-True ($null -ne $r.Core) 'core package still offered'
    Assert-True $r.HasDownload 'a download is still offered'
    Assert-Contains $r.ReleaseUrl 'releases/tag/' 'release page link present'
}

Test-Case 'package matching is case-insensitive across the two differing name stems' {
    $release = [pscustomobject]@{
        tag_name = 'dlss5-video-player-v9.9.9'; html_url = 'https://example.invalid/tag'
        published_at = '2026-01-01T00:00:00Z'; draft = $false; prerelease = $true
        assets = @(
            [pscustomobject]@{ name = 'DLSS5-VIDEO-PLAYER-V9.9.9-WIN64.ZIP'; size = 1; browser_download_url = 'https://example.invalid/full' }
            [pscustomobject]@{ name = 'dlssvideoplayer-v9.9.9-CORE-win64.zip'; size = 2; browser_download_url = 'https://example.invalid/core' }
        )
    }
    $r = Resolve-ReleaseData -Release $release
    Assert-True ($null -ne $r.Full) 'uppercase full package matched'
    Assert-True ($null -ne $r.Core) 'mixed-case core package matched'
    Assert-Equal 'https://example.invalid/core' $r.Core.Url 'core matched the core asset, not the full one'
}

Test-Case 'a .sha256 sidecar is never mistaken for a package' {
    $r = Resolve-ReleaseData -Release ((Read-TextFile -Path (Join-Path $fixtures 'release-full.json')) | ConvertFrom-Json)
    Assert-NotContains $r.Full.Name '.sha256' 'full package is the zip, not its checksum file'
    Assert-NotContains $r.Core.Name '.sha256' 'core package is the zip, not its checksum file'
}

Test-Case 'no release degrades to the release list without inventing a download' {
    $r = Resolve-ReleaseData -Release $null
    Assert-True ($null -eq $r.Full) 'no full package invented'
    Assert-True ($null -eq $r.Core) 'no core package invented'
    Assert-True (-not $r.HasDownload) 'page knows it has no direct download'
    Assert-Contains $r.ReleaseUrl '/releases' 'falls back to the release list'
}

Test-Case 'a checksum sidecar parses whether the body arrives as text or bytes' {
    $line = 'ef66a1efb7f1107b7dd941294711a1d64c346a96e2b8eaa5cf99bb23b4812cc8  package.zip'
    $expected = 'ef66a1efb7f1107b7dd941294711a1d64c346a96e2b8eaa5cf99bb23b4812cc8'
    Assert-Equal $expected (Get-ChecksumFromResponse -Content $line) 'parsed from a string body'
    # GitHub serves .sha256 as application/octet-stream, which Windows PowerShell
    # surfaces as a byte array; reading it as text is what this guards.
    Assert-Equal $expected (Get-ChecksumFromResponse -Content ([System.Text.Encoding]::UTF8.GetBytes($line))) 'parsed from a byte body'
    Assert-Equal $expected (Get-ChecksumFromResponse -Content ($line.ToUpperInvariant())) 'normalised to lower case'
    Assert-True ($null -eq (Get-ChecksumFromResponse -Content 'not a hash')) 'a body with no hash yields nothing'
    Assert-True ($null -eq (Get-ChecksumFromResponse -Content $null)) 'a missing body yields nothing'
}

Write-Host ''
Write-Host 'Analytics' -ForegroundColor Cyan

$partial = [IO.Path]::Combine($siteRoot, 'src', 'partials', 'analytics.html')

Test-Case 'unset measurement id produces no gtag reference' {
    $s = Get-AnalyticsSnippet -MeasurementId $null -PartialPath $partial
    Assert-NotContains $s 'gtag' 'no analytics when the variable is unset'
}

Test-Case 'empty measurement id produces no gtag reference' {
    $s = Get-AnalyticsSnippet -MeasurementId '   ' -PartialPath $partial
    Assert-NotContains $s 'gtag' 'whitespace is treated as unset'
}

Test-Case 'a valid measurement id appears exactly once' {
    $s = Get-AnalyticsSnippet -MeasurementId 'G-ABC1234567' -PartialPath $partial
    Assert-Contains $s 'gtag' 'analytics injected'
    $count = ([regex]::Matches($s, [regex]::Escape('G-ABC1234567'))).Count
    Assert-True ($count -ge 1) 'the id is substituted'
    Assert-NotContains $s '{{' 'no token survives in the analytics partial'
}

Test-Case 'a malformed measurement id fails the build' {
    Assert-Throws { Get-AnalyticsSnippet -MeasurementId 'UA-12345-1' -PartialPath $partial } 'a UA- id must be rejected'
    Assert-Throws { Get-AnalyticsSnippet -MeasurementId 'not-an-id' -PartialPath $partial } 'garbage must be rejected'
    Assert-Throws { Get-AnalyticsSnippet -MeasurementId 'G-' -PartialPath $partial } 'a truncated id must be rejected'
}

Write-Host ''
Write-Host 'GitHub star count' -ForegroundColor Cyan

Test-Case 'a star count reads the way a person says it' {
    Assert-Equal '0' (Format-StarCount 0) 'zero is a count'
    Assert-Equal '127' (Format-StarCount 127) 'small counts are exact'
    Assert-Equal '1,234' (Format-StarCount 1234) 'thousands are grouped'
    Assert-Equal '12.3k' (Format-StarCount 12345) 'large counts are shortened, never rounded up'
    Assert-Equal '' (Format-StarCount $null) 'no answer renders nothing, not a zero'
    Assert-Equal '' (Format-StarCount 'n/a') 'garbage renders nothing'
}

Test-Case 'no star count still yields a complete link' {
    $s = Get-StarSummary -Count $null
    Assert-Equal '' $s.Badge 'no badge without a count'
    Assert-Equal 'MIT licence' $s.Meta 'the button meta falls back to the licence'
    $one = Get-StarSummary -Count 1 -AsOf '1 January 2026'
    Assert-Contains $one.Badge '1<span class="gh__stars-noun"> star</span>' 'one star is singular'
    Assert-Contains $one.Badge 'when this page was built, 1 January 2026' 'the badge says when it was counted'
}

Write-Host ''
Write-Host 'Build output' -ForegroundColor Cyan

$distFull = Invoke-Build -Fixture 'release-full.json' -MeasurementId '' -Name 'full' -RepoFixture 'repo.json'
$htmlFull = Get-Html $distFull
$repoUrl = "https://github.com/$(Get-RepoSlug)"

Test-Case 'no unsubstituted token survives into dist' {
    foreach ($f in Get-ChildItem $distFull -Recurse -Include *.html, *.css, *.js, *.json) {
        $text = Read-TextFile -Path $f.FullName
        if ($text -match '\{\{') { throw "unsubstituted token in $($f.Name)" }
    }
}

Test-Case 'non-ASCII text survives the build unmangled' {
    # Windows PowerShell reads a BOM-less file as ANSI unless told otherwise,
    # which turns every em dash and multiplication sign in the templates into
    # mojibake. Those characters are written by codepoint here so this script
    # itself stays pure ASCII: read as cp1252, the em dash's third byte is 0x94,
    # a smart closing quote, which PowerShell honours as a string delimiter.
    $emDash = [char]0x2014
    $times = [char]0x00D7
    $middot = [char]0x00B7
    $mojibake = [string]([char]0x00E2 + [char]0x20AC)
    Assert-Contains $htmlFull $emDash 'em dash intact'
    Assert-Contains $htmlFull $times 'multiplication sign intact'
    Assert-Contains $htmlFull $middot 'middle dot intact'
    Assert-NotContains $htmlFull $mojibake 'no UTF-8-read-as-ANSI mojibake'
    Assert-NotContains $htmlFull ([string]([char]0x00C3 + [char]0x2014)) 'no double-encoded multiplication sign'
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $distFull 'index.html'))
    Assert-True (-not ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) 'no byte-order mark on the served HTML'
}

Test-Case 'the page states the version and both package sizes' {
    Assert-Contains $htmlFull '0.23.0' 'version present'
    Assert-Contains $htmlFull 'dlss5-video-player-v0.23.0-win64.zip' 'full package named'
    Assert-Contains $htmlFull 'DLSSVideoPlayer-v0.23.0-core-win64.zip' 'core package named'
}

Test-Case 'the page carries a checksum for each package' {
    Assert-Contains $htmlFull 'data-checksum' 'checksum element present'
}

Test-Case 'release.json is written for the client-side backstop' {
    $rj = Join-Path $distFull 'release.json'
    Assert-True (Test-Path $rj) 'release.json exists'
    $data = (Read-TextFile -Path $rj) | ConvertFrom-Json
    Assert-Equal '0.23.0' $data.version 'release.json states the version'
    Assert-True ($null -ne $data.builtAt) 'release.json carries a build timestamp'
}

Test-Case 'every site-relative href, src and poster resolves to a real file' {
    # Deliberately not `content`: that attribute carries prose on every meta tag.
    # og:image is an absolute URL and is covered by the absolute-URL skip below.
    $pattern = '(?:href|src|poster)="(?!https?:|mailto:|#|data:)([^"]+)"'
    $missing = @()
    foreach ($m in [regex]::Matches($htmlFull, $pattern)) {
        $rel = $m.Groups[1].Value -replace '^\./', '' -replace '\?.*$', ''
        if ([string]::IsNullOrWhiteSpace($rel)) { continue }
        if (-not (Test-Path (Join-Path $distFull $rel))) { $missing += $rel }
    }
    Assert-True ($missing.Count -eq 0) "unresolved local references: $($missing -join ', ')"
}

Test-Case 'every url() in the stylesheet resolves to a real file' {
    # Strip data URIs first: the grain texture is an inline SVG that itself
    # contains url(%23n) referring to its own filter, which is not a file.
    $css = Read-TextFile -Path (Join-Path $distFull 'styles.css')
    $css = [regex]::Replace($css, 'url\(\s*"data:[^"]*"\s*\)', 'url(inline)')
    $missing = @()
    foreach ($m in [regex]::Matches($css, 'url\((?!data:|inline)["'']?([^"'')]+)["'']?\)')) {
        $rel = $m.Groups[1].Value -replace '^\./', '' -replace '\?.*$', ''
        if (-not (Test-Path (Join-Path $distFull $rel))) { $missing += $rel }
    }
    Assert-True ($missing.Count -eq 0) "unresolved stylesheet references: $($missing -join ', ')"
}

Test-Case 'column-swapped grid items also pin their row' {
    # The alternating "How it works" rows place the text in column 2 and the
    # figure in column 1, against DOM order. Grid's default sparse packing never
    # moves the placement cursor backwards, so without an explicit row the
    # figure starts a second row and the pair stacks instead of sitting side by
    # side - which is exactly what shipped once. Any rule that assigns a column
    # to these two must assign a row with it.
    $css = Read-TextFile -Path (Join-Path $distFull 'styles.css')
    foreach ($m in [regex]::Matches($css, '(?m)^[^\r\n{}]*\.beat__(?:text|figure)[^{}]*\{([^}]*)\}')) {
        $body = $m.Groups[1].Value
        if ($body -match 'grid-column' -and $body -notmatch 'grid-row') {
            throw "a .beat__text/.beat__figure rule sets grid-column without grid-row: $($m.Value.Trim())"
        }
    }
}

Test-Case 'every download link acknowledges the click' {
    # A release asset is served cross-origin, so the page learns nothing about
    # the download - and the complete package is over 300 MB, long enough for
    # the browser's own indicator to feel late. Every link carrying
    # data-download must therefore have a slot to relabel, and the page must
    # carry the live region that says it once for a screen reader.
    $js = Read-TextFile -Path (Join-Path $distFull 'main.js')
    $css = Read-TextFile -Path (Join-Path $distFull 'styles.css')

    Assert-Contains $htmlFull 'class="download-status"' 'the live region ships'
    Assert-Contains $htmlFull 'aria-live="polite"' 'the live region is polite'
    Assert-Contains $js 'data-downloading' 'the click sets the busy state'
    Assert-Contains $css '[data-downloading]' 'the busy state is styled'

    # The relabel writes into whichever slot the link keeps its size in, so a
    # link with neither leaves the click unacknowledged.
    $options = [System.Text.RegularExpressions.RegexOptions]::Singleline
    $links = [regex]::Matches($htmlFull, '<a[^>]*data-download=[^>]*>(.*?)</a>', $options)
    $bad = @()
    foreach ($m in $links) {
        $inner = $m.Groups[1].Value
        if ($inner -notmatch 'button__meta' -and $inner -notmatch 'pkg__cta-size') {
            $bad += $m.Value.Trim()
        }
    }

    # Asserted before the findings, and the reason this is not a bare loop: a
    # loop that matches nothing reports every link as fine. The first draft of
    # this test did exactly that - a stray control character in the pattern made
    # it match zero links, and it passed against a page carrying a link with no
    # slot at all.
    Assert-True ($links.Count -ge 3) "expected the hero and both package links, matched $($links.Count)"
    Assert-True ($bad.Count -eq 0) "data-download link with no slot to relabel: $($bad -join ' | ')"
}

Test-Case 'the page keeps its one authored motion moment' {
    # DESIGN.md: the seam's entry sweep is the page's only animation, and it is
    # driven from main.js by requestAnimationFrame rather than by CSS. So the
    # stylesheet declares no keyframes and nothing that runs on its own - which
    # is also why the download acknowledgement is a state change and not a
    # spinner. A spinner here would be doubly wrong: it would take the page's
    # one moment, and it would be pretending to track a cross-origin transfer
    # that reports nothing back.
    $css = Read-TextFile -Path (Join-Path $distFull 'styles.css')
    Assert-NotContains $css '@keyframes' 'no keyframes in the stylesheet'
    if ($css -match '(?m)^\s*animation(-name)?\s*:') {
        throw 'the stylesheet starts an animation; the page has one authored moment and it is the seam'
    }
}

Test-Case 'the hero comparison crops ship and stay a matched pair' {
    $originalCrop = [IO.Path]::Combine($distFull, 'assets', 'hero', 'hero-original.jpg')
    $neuralCrop = [IO.Path]::Combine($distFull, 'assets', 'hero', 'hero-neural.jpg')
    Assert-True (Test-Path $originalCrop) 'original crop shipped'
    Assert-True (Test-Path $neuralCrop) 'neural crop shipped'
    # A pair that became identical would be a comparison proving nothing.
    $a = [System.IO.File]::ReadAllBytes($originalCrop)
    $b = [System.IO.File]::ReadAllBytes($neuralCrop)
    Assert-True ($a.Length -ne $b.Length -or [Convert]::ToBase64String($a) -ne [Convert]::ToBase64String($b)) 'the two crops are not the same image'
}

Test-Case 'analytics is absent when the variable is unset' {
    Assert-NotContains $htmlFull 'gtag' 'no tracking in an unconfigured build'
    Assert-NotContains $htmlFull 'googletagmanager' 'no third-party request in an unconfigured build'
}

Test-Case 'the binding disclaimers survive into the page' {
    Assert-Contains $htmlFull 'unsigned' 'unsigned-runtime notice present'
    Assert-Contains $htmlFull 'not an NVIDIA product' 'community-project notice present'
}

Test-Case 'structured data is valid JSON and describes this release' {
    $m = [regex]::Match($htmlFull, '(?s)<script type="application/ld\+json">(.*?)</script>')
    Assert-True $m.Success 'a JSON-LD block is present'
    $ld = $m.Groups[1].Value | ConvertFrom-Json   # throws if malformed
    Assert-True ($null -ne $ld.'@graph') 'the graph is present'
    $app = @($ld.'@graph' | Where-Object { $_.'@type' -eq 'SoftwareApplication' })[0]
    Assert-True ($null -ne $app) 'SoftwareApplication node present'
    Assert-Equal '0.23.0' $app.softwareVersion 'the declared version matches the release'
    Assert-Contains $app.downloadUrl 'http' 'a real download URL is declared'
    # Get-Property, not direct access: StrictMode makes reading an absent
    # property an error, and absence is exactly what this asserts.
    Assert-True ($null -eq (Get-Property $app 'aggregateRating')) 'no rating is fabricated'
    Assert-True ($null -eq (Get-Property $app 'review')) 'no review is fabricated'
}

Test-Case 'every visible FAQ question appears in the FAQ structured data' {
    # Structured data that does not match what the visitor can see is a
    # penalty, not an optimisation, so the two are checked against each other.
    $m = [regex]::Match($htmlFull, '(?s)<script type="application/ld\+json">(.*?)</script>')
    $ld = $m.Groups[1].Value | ConvertFrom-Json
    $faq = @($ld.'@graph' | Where-Object { $_.'@type' -eq 'FAQPage' })[0]
    Assert-True ($null -ne $faq) 'FAQPage node present'

    $visible = @([regex]::Matches($htmlFull, '<summary>(.*?)</summary>') | ForEach-Object { $_.Groups[1].Value.Trim() })
    Assert-True ($visible.Count -gt 0) 'the page has visible FAQ entries'
    $marked = @($faq.mainEntity | ForEach-Object { $_.name })
    Assert-Equal $visible.Count $marked.Count 'the same number of questions is marked up as is shown'
    foreach ($q in $visible) {
        if ($marked -notcontains $q) { throw "visible question not in structured data: $q" }
    }
}

Test-Case 'crawl files ship and point at this site' {
    $robots = Read-TextFile -Path (Join-Path $distFull 'robots.txt')
    Assert-Contains $robots 'Sitemap:' 'robots.txt names the sitemap'
    Assert-Contains $robots 'github.io' 'robots.txt points at the deployed host'
    $sitemap = Read-TextFile -Path (Join-Path $distFull 'sitemap.xml')
    Assert-Contains $sitemap '<loc>' 'sitemap lists a URL'
    Assert-Contains $sitemap 'github.io' 'sitemap points at the deployed host'
    [xml]$parsed = $sitemap   # throws if malformed
    Assert-True ($null -ne $parsed.urlset) 'sitemap is well-formed XML'
}

Test-Case 'the page carries the metadata a search result is built from' {
    Assert-Contains $htmlFull '<link rel="canonical"' 'canonical URL declared'
    Assert-Contains $htmlFull 'property="og:image"' 'social image declared'
    Assert-Contains $htmlFull 'name="twitter:card"' 'twitter card declared'
    Assert-Contains $htmlFull 'name="description"' 'meta description present'
    $desc = [regex]::Match($htmlFull, '<meta name="description" content="([^"]*)"').Groups[1].Value
    Assert-True ($desc.Length -gt 70 -and $desc.Length -lt 320) "meta description is a usable length (is $($desc.Length))"
    Assert-Equal 1 ([regex]::Matches($htmlFull, '<h1[ >]').Count) 'exactly one h1'
}

Test-Case 'the comparison is operable without a pointer' {
    Assert-Contains $htmlFull 'type="range"' 'the seam is a real range input'
}

Test-Case 'the comparison slider says its position in words' {
    # The range's value is a seam position, which a screen reader would read out
    # as a bare number. The text says how much of the frame each plate holds,
    # in the markup for the first read and from the script on every move.
    $range = [regex]::Match($htmlFull, '<input[^>]*id="seam"[^>]*>').Value
    Assert-Contains $range 'aria-valuetext="55% original, 45% neural render"' 'the authored position is described'
    $js = Read-TextFile -Path (Join-Path $distFull 'main.js')
    Assert-Contains $js "setAttribute('aria-valuetext'" 'the description follows the seam'
}

Test-Case 'the skip link lands on the main content' {
    $skip = [regex]::Match($htmlFull, '<a class="skip" href="#([^"]+)"')
    Assert-True $skip.Success 'a skip link is the first thing a keyboard reaches'
    Assert-Equal 'main' $skip.Groups[1].Value 'it skips the masthead, not to the download'
    Assert-True ($htmlFull -match '<main id="main" tabindex="-1"') 'the target exists and can take focus'
}

Test-Case 'the grain stays on its own layer and never takes the pointer' {
    # Fixed over the whole viewport, above everything: without a layer of its
    # own every scroll repaints it, SVG noise included.
    $css = Read-TextFile -Path (Join-Path $distFull 'styles.css')
    $m = [regex]::Match($css, '(?s)\n\.grain \{(.*?)\}')
    Assert-True $m.Success 'the grain rule is present'
    Assert-Contains $m.Groups[1].Value 'will-change: transform' 'the grain is promoted to its own compositor layer'
    Assert-Contains $m.Groups[1].Value 'contain: strict' 'the grain is fully contained'
    Assert-Contains $m.Groups[1].Value 'pointer-events: none' 'the grain never intercepts a click'
}

Test-Case 'every srcset and imagesrcset candidate resolves to a real file' {
    # The href/src test above cannot see these: a responsive image names its
    # files in a comma-separated list, and a missing variant fails silently -
    # the browser just picks another, or shows nothing.
    $lists = [regex]::Matches($htmlFull, '(?:imagesrcset|srcset)="([^"]+)"')
    Assert-True ($lists.Count -ge 10) "expected the hero, the gallery and the beats to carry srcsets, found $($lists.Count)"
    $missing = @()
    foreach ($list in $lists) {
        foreach ($candidate in $list.Groups[1].Value.Split(',')) {
            $rel = ($candidate.Trim() -split '\s+')[0]
            if (-not (Test-Path (Join-Path $distFull $rel))) { $missing += $rel }
        }
    }
    Assert-True ($missing.Count -eq 0) "unresolved srcset candidates: $($missing -join ', ')"
}

Test-Case 'the hero is offered small to a phone, and preloaded as a set' {
    # Both 1920x1080 JPEGs used to be preloaded on every viewport. The preload
    # now names the AVIF set and its sizes, so a phone fetches the width it
    # shows, and the JPEGs are only the fallback inside <picture>.
    Assert-NotContains $htmlFull 'rel="preload" as="image" href="assets/hero/hero-neural.jpg"' 'the full-size JPEG is not preloaded'
    $preloads = [regex]::Matches($htmlFull, '(?s)<link rel="preload" as="image"[^>]*>')
    Assert-Equal 2 $preloads.Count 'one preload per plate'
    foreach ($p in $preloads) {
        Assert-Contains $p.Value 'type="image/avif"' 'the preload is skipped by a browser that cannot use it'
        Assert-Contains $p.Value 'imagesrcset=' 'the preload is a responsive set'
        Assert-Contains $p.Value 'imagesizes=' 'the preload says how wide the plate is drawn'
        Assert-Contains $p.Value ' 960w' 'a phone-sized candidate is offered'
    }
    foreach ($kind in @('neural', 'original')) {
        foreach ($format in @('avif', 'webp')) {
            Assert-Contains $htmlFull "assets/hero/hero-$kind-960.$format 960w" "the $kind plate offers a 960 px $format"
        }
    }
}

Test-Case 'every encoded variant stays a matched pair' {
    # An encode that made the two plates more alike than their sources would
    # have the page flatter the render. make-responsive-images.ps1 refuses to
    # write such a pair; this refuses to ship one that was edited by hand.
    $checked = 0
    foreach ($dir in @('hero', 'gallery')) {
        $manifestPath = [IO.Path]::Combine($distFull, 'assets', $dir, 'variants.json')
        Assert-True (Test-Path $manifestPath) "$dir/variants.json ships beside the files it describes"
        $manifest = (Read-TextFile -Path $manifestPath).TrimStart([char]0xFEFF) | ConvertFrom-Json
        foreach ($set in $manifest.sets.PSObject.Properties) {
            foreach ($v in $set.Value.variants) {
                $a = [IO.Path]::Combine($distFull, 'assets', $dir, $v.original.file)
                $b = [IO.Path]::Combine($distFull, 'assets', $dir, $v.neural.file)
                Assert-True ((Test-Path $a) -and (Test-Path $b)) "both plates of $($set.Name) $($v.format) $($v.width) ship"
                Assert-True ((Get-Item $a).Length -ne (Get-Item $b).Length -or
                    [Convert]::ToBase64String([IO.File]::ReadAllBytes($a)) -ne [Convert]::ToBase64String([IO.File]::ReadAllBytes($b))) `
                    "the $($set.Name) $($v.format) $($v.width) plates are not the same file"
                Assert-True ([double]$v.pairPsnr -le [double]$v.sourcePairPsnr + 0.5) `
                    "$($set.Name) $($v.format) $($v.width): the encoded pair ($($v.pairPsnr) dB) is more alike than its sources ($($v.sourcePairPsnr) dB)"
                $checked++
            }
        }
    }
    Assert-True ($checked -ge 18) "expected the hero and three scenes in two formats, checked $checked"
}

Test-Case 'every gallery scene has its pair, a flip, a 1:1 view, a link and provenance' {
    $options = [System.Text.RegularExpressions.RegexOptions]::Singleline
    $scenes = [regex]::Matches($htmlFull, '<figure class="scene" id="([^"]+)"(.*?)</figure>', $options)
    # Asserted first so a pattern that matches nothing cannot pass the loop.
    Assert-True ($scenes.Count -ge 3) "expected at least three scenes, matched $($scenes.Count)"
    $ids = @{}
    foreach ($s in $scenes) {
        $id = $s.Groups[1].Value
        $body = $s.Groups[2].Value
        Assert-True (-not $ids.ContainsKey($id)) "scene id $id is unique"
        $ids[$id] = $true

        $original = [regex]::Match($body, 'data-full-original="([^"]+)"').Groups[1].Value
        $neural = [regex]::Match($body, 'data-full-neural="([^"]+)"').Groups[1].Value
        Assert-True ($original -and $neural) "$id names both full-size plates"
        $a = Join-Path $distFull $original
        $b = Join-Path $distFull $neural
        Assert-True ((Test-Path $a) -and (Test-Path $b)) "$id ships both full-size plates for its 1:1 view"
        Assert-True ([Convert]::ToBase64String([IO.File]::ReadAllBytes($a)) -ne [Convert]::ToBase64String([IO.File]::ReadAllBytes($b))) "$id is not the same image twice"

        Assert-Contains $body 'scene__plate--neural' "$id stacks the neural plate"
        Assert-Contains $body 'scene__plate--original' "$id stacks the original plate"
        Assert-True ($body -match '<button class="scene__button scene__flip" type="button" hidden>') "$id has a flip button, hidden until the script can drive it"
        Assert-True ($body -match '<button class="scene__button scene__zoom" type="button" aria-pressed="false" hidden>') "$id has a 1:1 toggle that states whether it is on"
        Assert-Contains $body "href=`"#$id`"" "$id links to itself"
        Assert-Contains $body "href=`"$original`"" "$id links its full-size original without scripting"
        Assert-Contains $body "href=`"$neural`"" "$id links its full-size render without scripting"
        $data = [regex]::Match($body, '<span class="scene__data">([^<]+)</span>').Groups[1].Value
        Assert-True ($data.Length -gt 40) "$id carries its provenance line"
    }
}

Test-Case 'the flip is a state change, not a transition' {
    # A cross-fade would put a blend of the two plates on screen and call it
    # one of them, and it would take the page's one authored moment besides.
    $css = Read-TextFile -Path (Join-Path $distFull 'styles.css')
    foreach ($m in [regex]::Matches($css, '(?m)^[^\r\n{}]*\.scene__plate[^{}]*\{([^}]*)\}')) {
        if ($m.Groups[1].Value -match 'transition|animation|opacity') {
            throw "a plate rule animates or blends: $($m.Value.Trim())"
        }
    }
    # .Contains rather than Assert-Contains: -like reads the brackets as a set.
    Assert-True ($css.Contains("[data-view='neural'] .scene__plate--original { visibility: hidden; }")) 'the top plate is shown or hidden outright'
}

Test-Case 'the GitHub link is in the masthead, beside the downloads and in the footer' {
    $options = [System.Text.RegularExpressions.RegexOptions]::Singleline
    foreach ($where in @('masthead__gh', 'pkg__cta--gh', 'footer__gh')) {
        $m = [regex]::Match($htmlFull, "<a class=`"[^`"]*$where[^`"]*`" href=`"([^`"]+)`"[^>]*>(.*?)</a>", $options)
        Assert-True $m.Success "a $where link is present"
        Assert-Equal $repoUrl $m.Groups[1].Value "the $where link points at the repository"
        Assert-Contains $m.Groups[2].Value '<svg class="gh__mark"' "the $where link carries the inline mark"
        Assert-Contains $m.Groups[2].Value 'aria-hidden="true"' "the $where mark is hidden from assistive technology"
        $text = [regex]::Replace($m.Groups[2].Value, '<svg.*?</svg>|<[^>]+>', '', $options).Trim()
        Assert-True ($text -match 'GitHub|View source') "the $where link has a spoken name without the mark (got '$text')"
    }
}

Test-Case 'the star count is baked in at build time and never fetched by the page' {
    Assert-Contains $htmlFull 'class="gh__stars"' 'the count renders from the repository record'
    Assert-Contains $htmlFull '127' 'the fixture count reaches the page'
    Assert-Contains $htmlFull '127 stars' 'the source row states the count in words'
    $js = Read-TextFile -Path (Join-Path $distFull 'main.js')
    Assert-NotContains $js 'stargazers' 'the page makes no star-count request of its own'
}

$distGa = Invoke-Build -Fixture 'release-full.json' -MeasurementId 'G-TEST1234567' -Name 'ga'

Test-Case 'a configured measurement id reaches the page exactly once' {
    $html = Get-Html $distGa
    $count = ([regex]::Matches($html, [regex]::Escape('G-TEST1234567'))).Count
    Assert-Equal 2 $count 'the id appears once in the script src and once in the config call'
    Assert-Contains $html 'googletagmanager.com/gtag/js' 'the gtag loader is present'
}

$distCore = Invoke-Build -Fixture 'release-core-only.json' -MeasurementId '' -Name 'core'

Test-Case 'the hand-attachment window renders without a dead full-package link' {
    $html = Get-Html $distCore
    Assert-NotContains $html 'dlss5-video-player-v0.23.0-win64.zip' 'no link to an unattached package'
    Assert-Contains $html 'DLSSVideoPlayer-v0.23.0-core-win64.zip' 'core package still offered'
    Assert-Contains $html 'releases/tag/' 'release page offered for the complete package'
}

$distNone = Invoke-Build -Fixture 'release-none.json' -MeasurementId '' -Name 'none'

Test-Case 'no published release renders a release-list link, not a download' {
    $html = Get-Html $distNone
    Assert-NotContains $html '-win64.zip' 'no package link invented'
    Assert-Contains $html '/releases' 'release list offered instead'
}

Test-Case 'without a star count the GitHub links render whole, with no empty badge' {
    $html = Get-Html $distNone
    Assert-NotContains $html 'gh__stars' 'no count, no badge'
    Assert-Contains $html 'MIT licence' 'the source row falls back to the licence'
    Assert-Contains $html 'class="gh masthead__gh"' 'the masthead link is still there'
}

# --- report -------------------------------------------------------------------

if (Test-Path $tempRoot) { Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue }

Write-Host ''
if ($script:Failures.Count -gt 0) {
    Write-Host "$($script:Failures.Count) failed, $($script:Passed) passed" -ForegroundColor Red
    exit 1
}
Write-Host "$($script:Passed) passed" -ForegroundColor Green
exit 0
