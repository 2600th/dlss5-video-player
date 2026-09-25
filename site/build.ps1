#Requires -Version 5.1
<#
.SYNOPSIS
    Renders the project website into site/dist.

.DESCRIPTION
    Resolves the current published release, substitutes it into the template
    along with the analytics tag, and copies the page's media out of docs/ so
    there is only ever one copy of a screenshot in the repository.

.PARAMETER ReleaseFixture
    Read the release from a JSON file instead of the GitHub API, and make no
    network requests at all. This is how test.ps1 exercises the states a tag
    would otherwise have to exist to produce.

.EXAMPLE
    .\site\build.ps1
    Builds against the live API and writes site/dist.

.EXAMPLE
    .\site\build.ps1 -ReleaseFixture site/fixtures/release-core-only.json
    Builds the state where CI has published a release but the complete package
    has not been attached to it yet.

.PARAMETER RepoFixture
    Read the repository record (its star count) from a JSON file instead of the
    GitHub API. With -ReleaseFixture and no -RepoFixture the build makes no
    request and renders the GitHub links without a star count.
#>
[CmdletBinding()]
param(
    [string]$OutputPath,
    [string]$ReleaseFixture,
    [string]$RepoFixture,
    [switch]$SkipMedia,
    [switch]$RequireRelease,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$siteRoot = $PSScriptRoot
$repoRoot = Split-Path $siteRoot -Parent
$srcRoot = Join-Path $siteRoot 'src'
. ([IO.Path]::Combine($siteRoot, 'lib', 'Site.ps1'))

if (-not $OutputPath) { $OutputPath = Join-Path $siteRoot 'dist' }

function Write-Step { param([string]$Message)
    if (-not $Quiet) { Write-Host "  $Message" -ForegroundColor DarkGray } }

$repo = Get-RepoSlug
$siteUrl = "https://$($repo.Split('/')[0]).github.io/$($repo.Split('/')[1])/"

# --- 1. resolve the release ---------------------------------------------------

function Invoke-GitHubApi {
    param([string]$Path)
    $headers = @{ 'User-Agent' = 'dlss5-video-player-site'; 'Accept' = 'application/vnd.github+json' }
    $token = if ($env:GITHUB_TOKEN) { $env:GITHUB_TOKEN } elseif ($env:GH_TOKEN) { $env:GH_TOKEN } else { $null }
    if ($token) { $headers['Authorization'] = "Bearer $token" }
    try {
        return Invoke-RestMethod -Uri "https://api.github.com/$Path" -Headers $headers -ErrorAction Stop
    } catch {
        Write-Step "api: $Path -> $($_.Exception.Message)"
        return $null
    }
}

function Get-CurrentRelease {
    <#  Resolution order, and why it is not /releases/latest:

        Every release this project has published is marked as a prerelease, and
        GET /releases/latest excludes prereleases, so that endpoint returns 404
        here and the equivalent web URL redirects to the release list. Resolving
        by tag instead reuses the convention release.yml already enforces, so
        the site and the release gate cannot disagree about the current version.
    #>
    $versionFile = Join-Path $repoRoot 'VERSION'
    if (Test-Path $versionFile) {
        $version = (Read-TextFile -Path $versionFile).Trim()
        $tag = "dlss5-video-player-v$version"
        Write-Step "resolving $tag"
        $byTag = Invoke-GitHubApi "repos/$repo/releases/tags/$tag"
        if ($null -ne $byTag -and -not (Get-Property $byTag 'draft' $false)) { return $byTag }
        Write-Step "$tag is not published yet; falling back to the newest release"
    }

    $all = Invoke-GitHubApi "repos/$repo/releases?per_page=30"
    if ($null -eq $all) { return $null }
    return @($all | Where-Object { -not (Get-Property $_ 'draft' $false) } |
        Sort-Object { [datetime](Get-Property $_ 'published_at' '1970-01-01') } -Descending) |
        Select-Object -First 1
}

if ($ReleaseFixture) {
    Write-Step "release from fixture $(Split-Path $ReleaseFixture -Leaf)"
    $raw = (Read-TextFile -Path $ReleaseFixture) | ConvertFrom-Json
    $release = Resolve-ReleaseData -Release $raw -Repo $repo
} else {
    $release = Resolve-ReleaseData -Release (Get-CurrentRelease) -Repo $repo

    # The checksum is the reason to trust a 308 MB download from a community
    # project, so it goes on the page rather than one click away. Failing to
    # fetch it is not failing to build: the page falls back to linking the
    # sidecar file.
    foreach ($pkg in @($release.Full, $release.Core)) {
        if ($null -eq $pkg -or -not $pkg.Sha256Url) { continue }
        try {
            $response = Invoke-WebRequest -Uri $pkg.Sha256Url -UseBasicParsing `
                -Headers @{ 'User-Agent' = 'dlss5-video-player-site' }
            $pkg.Sha256 = Get-ChecksumFromResponse -Content $response.Content
            if (-not $pkg.Sha256) { Write-Step "checksum for $($pkg.Name) did not parse; linking the sidecar instead" }
        } catch {
            Write-Step "checksum for $($pkg.Name) unavailable: $($_.Exception.Message)"
        }
    }
}

if ($release.HasDownload) {
    Write-Step "release $($release.Tag)"
} elseif ($RequireRelease) {
    # The release-list fallback exists so an unexpected state still renders a
    # usable page. It is not something to publish on purpose: in CI, a build
    # that could not resolve the download fails instead of quietly replacing
    # the download button with a link to a list of 17 releases.
    throw 'No published release could be resolved, and -RequireRelease was set. Refusing to deploy a page with no download.'
} else {
    Write-Step 'no published release resolved; the page will link the release list'
}

# --- 1b. the repository's star count ------------------------------------------

# Fetched here, at build time, and never by the page: a static number costs the
# visitor nothing and cannot rate-limit. Failing to fetch it is not failing to
# build - the links render without a count.
$repoRecord = $null
if ($RepoFixture) {
    $repoRecord = (Read-TextFile -Path $RepoFixture) | ConvertFrom-Json
} elseif (-not $ReleaseFixture) {
    $repoRecord = Invoke-GitHubApi "repos/$repo"
}
$builtOn = (Get-Date).ToUniversalTime().ToString('d MMMM yyyy', [cultureinfo]::InvariantCulture)
$stars = Get-StarSummary -Count (Get-Property $repoRecord 'stargazers_count' $null) -AsOf $builtOn
Write-Step $(if ($stars.Text) { "github: $($stars.Text)" } else { 'github: no star count; links render without one' })

# --- 2. analytics -------------------------------------------------------------

$analytics = Get-AnalyticsSnippet -MeasurementId $env:GA_MEASUREMENT_ID `
    -PartialPath ([IO.Path]::Combine($srcRoot, 'partials', 'analytics.html'))
Write-Step $(if ($analytics -like '*gtag*') { 'analytics enabled' } else { 'analytics disabled' })

# --- 3. render ----------------------------------------------------------------

$packageTemplate = Read-TextFile -Path ([IO.Path]::Combine($srcRoot, 'partials', 'package.html'))

function New-PackageBlock {
    param($Package, [string]$Kind, [string]$Summary, [string]$EventName, [bool]$Primary)
    if ($null -eq $Package) { return '' }

    $checksum = if ($Package.Sha256) { $Package.Sha256 } else { '' }
    $checksumMarkup = if ($Package.Sha256) {
        "<button class=`"pkg__sum`" type=`"button`" data-checksum=`"$($Package.Sha256)`" title=`"Copy SHA-256`"><span class=`"pkg__sum-value`">$($Package.Sha256.Substring(0,16))&#8230;</span><span class=`"pkg__sum-action`">copy sha-256</span></button>"
    } elseif ($Package.Sha256Url) {
        "<a class=`"pkg__sum`" data-checksum=`"`" href=`"$($Package.Sha256Url)`"><span class=`"pkg__sum-value`">sha-256</span><span class=`"pkg__sum-action`">sidecar</span></a>"
    } else { '' }

    Expand-Token -Text $packageTemplate -Values @{
        PKG_KIND     = $Kind
        PKG_NAME     = ConvertTo-HtmlText $Package.Name
        PKG_URL      = $Package.Url
        PKG_SIZE     = $Package.SizeText
        PKG_SUMMARY  = $Summary
        PKG_CHECKSUM = $checksumMarkup
        PKG_EVENT    = $EventName
        PKG_CLASS    = $(if ($Primary) { 'pkg pkg--primary' } else { 'pkg' })
        PKG_CTA      = $(if ($Primary) { 'Download' } else { 'Download' })
    }
}

$fullBlock = New-PackageBlock -Package $release.Full -Kind 'Complete' `
    -Summary 'The player and the pinned neural runtime. This is the one you want.' `
    -EventName 'full' -Primary $true

$coreBlock = New-PackageBlock -Package $release.Core -Kind 'Core' `
    -Summary 'The player alone, without the neural runtime. Build or supply your own.' `
    -EventName 'core' -Primary $false

# The source sits beside the downloads as a package of its own: the secondary
# action, in the same row shape, so it reads as an option rather than an ad.
# One copy of the mark's path, as a symbol at the top of the page, and a
# <use> of it wherever it appears: no icon font, no request, and the 1 KB path
# is sent once rather than four times.
$githubMark = (Read-TextFile -Path ([IO.Path]::Combine($srcRoot, 'partials', 'github-mark.svg'))).Trim()
$githubSymbol = (Read-TextFile -Path ([IO.Path]::Combine($srcRoot, 'partials', 'github-symbol.svg'))).Trim()

$sourceBlock = Expand-Token -Text (Read-TextFile -Path ([IO.Path]::Combine($srcRoot, 'partials', 'source.html'))) `
    -Values @{ REPO_URL = "https://github.com/$repo"; REPO_SLUG = $repo; GITHUB_META = $stars.Meta; GITHUB_MARK = $githubMark }

if (-not $release.HasDownload) {
    $fullBlock = Expand-Token -Text (Read-TextFile -Path ([IO.Path]::Combine($srcRoot, 'partials', 'no-release.html'))) `
        -Values @{ RELEASE_URL = $release.ReleaseUrl }
} elseif ($null -eq $release.Full) {
    # CI has published, but the complete package is still being attached by
    # hand. Say so, rather than linking a file that is not there.
    $fullBlock = Expand-Token -Text (Read-TextFile -Path ([IO.Path]::Combine($srcRoot, 'partials', 'pending-package.html'))) `
        -Values @{ RELEASE_URL = $release.ReleaseUrl }
}

$publishedIso = ''
if ($release.PublishedAt) {
    $publishedIso = ([datetime]$release.PublishedAt).ToUniversalTime().ToString('yyyy-MM-dd')
}

$publishedText = ''
if ($release.PublishedAt) {
    $publishedText = ([datetime]$release.PublishedAt).ToUniversalTime().ToString('d MMMM yyyy', [cultureinfo]::InvariantCulture)
}

$primaryUrl = if ($release.Full) { $release.Full.Url }
              elseif ($release.Core) { $release.Core.Url }
              else { $release.ReleaseUrl }
$primarySize = if ($release.Full) { $release.Full.SizeText }
               elseif ($release.Core) { $release.Core.SizeText }
               else { '' }

# The demonstration's size is read from the file, so the play button cannot
# promise a download of a different size from the one it starts.
$demoSource = Join-Path $repoRoot 'docs/media/neural-comparison-demo.mp4'
$demoSize = if (Test-Path $demoSource) { Format-ByteSize (Get-Item $demoSource).Length } else { '' }

$tokens = @{
    SITE_URL        = $siteUrl
    REPO_URL        = "https://github.com/$repo"
    VERSION         = $(if ($release.Version) { $release.Version } else { '' })
    VERSION_LABEL   = $(if ($release.Version) { "v$($release.Version)" } else { 'latest' })
    RELEASE_URL     = $release.ReleaseUrl
    RELEASE_DATE    = $publishedText
    PUBLISHED_ISO   = $publishedIso
    PRIMARY_URL     = $primaryUrl
    PRIMARY_SIZE    = $primarySize
    FULL_PACKAGE    = $fullBlock
    CORE_PACKAGE    = $coreBlock
    SOURCE_PACKAGE  = $sourceBlock
    REPO_SLUG       = $repo
    GITHUB_STARS    = $stars.Badge
    GITHUB_MARK     = $githubMark
    GITHUB_SYMBOL   = $githubSymbol
    GITHUB_META     = $stars.Meta
    DEMO_SIZE       = $demoSize
    ANALYTICS       = $analytics
    BUILT_AT        = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    YEAR            = (Get-Date).ToUniversalTime().Year
}

$html = Expand-Token -Text (Read-TextFile -Path (Join-Path $srcRoot 'index.html')) -Values $tokens

# The page marks what is in main but not yet released. v0.25.0 is the last
# release without those features; a build that offers a newer one drops the
# marks and the note, because they would then be false.
$html = Resolve-UnreleasedMarks -Html $html -ReleaseVersion $release.Version -LastWithout '0.25.0'

# --- 4. write -----------------------------------------------------------------

if (Test-Path $OutputPath) { Remove-Item $OutputPath -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutputPath | Out-Null

Write-TextFile -Path (Join-Path $OutputPath 'index.html') -Text $html

foreach ($file in @('styles.css', 'main.js')) {
    $text = Read-TextFile -Path (Join-Path $srcRoot $file)
    Write-TextFile -Path (Join-Path $OutputPath $file) -Text (Expand-Token -Text $text -Values $tokens)
}

Copy-Item (Join-Path $srcRoot 'assets') (Join-Path $OutputPath 'assets') -Recurse -Force

# GitHub Pages runs Jekyll over an artifact unless told not to; without this a
# directory whose name starts with an underscore would be dropped silently.
Write-TextFile -Path (Join-Path $OutputPath '.nojekyll') -Text ''

# --- crawl files --------------------------------------------------------------

Write-TextFile -Path (Join-Path $OutputPath 'robots.txt') -Text @"
User-agent: *
Allow: /

Sitemap: ${siteUrl}sitemap.xml
"@

# One page, but a sitemap still gives crawlers a lastmod to work from, and the
# date is the release's rather than the build's: rebuilding the site without a
# new release has not changed anything worth recrawling for. It also names the
# evidence images and the demonstration, which image and video search index
# from here rather than from a lazily loaded <picture>.
$lastMod = if ($publishedIso) { $publishedIso } else { (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd') }
$sitemapImages = @(
    'assets/hero/hero-neural.jpg'
    'assets/hero/hero-original.jpg'
    'media/007-first-light-bond.png'
    'media/007-first-light-suit.png'
    'media/resident-evil-requiem-flashlight.png'
    'media/gta6-lucia.png'
    'media/ac-shadows-low-key-limit.png'
    'media/compare-wipe.jpg'
    'media/compare-difference.jpg'
    'media/photo-wipe.jpg'
    'media/neural-playback.jpg'
) | ForEach-Object { "    <image:image><image:loc>$siteUrl$_</image:loc></image:image>" }
Write-TextFile -Path (Join-Path $OutputPath 'sitemap.xml') -Text @"
<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9"
        xmlns:image="http://www.google.com/schemas/sitemap-image/1.1"
        xmlns:video="http://www.google.com/schemas/sitemap-video/1.1">
  <url>
    <loc>$siteUrl</loc>
    <lastmod>$lastMod</lastmod>
    <changefreq>monthly</changefreq>
    <priority>1.0</priority>
$($sitemapImages -join "`n")
    <video:video>
      <video:thumbnail_loc>${siteUrl}media/neural-comparison-poster.jpg</video:thumbnail_loc>
      <video:title>DLSS 5 Video Player: the source against the player's DLSS 5 render, in 25 seconds</video:title>
      <video:description>007 First Light and Grand Theft Auto VI Trailer 2 at default settings: a source frame split against the same frame of the player's render, GTA VI playing with the divider, then the player's Difference, Side by side and loupe views and a render filling the timeline. No sound.</video:description>
      <video:content_loc>${siteUrl}media/neural-comparison-demo.mp4</video:content_loc>
      <video:duration>25</video:duration>
    </video:video>
  </url>
</urlset>
"@

# --- 5. media -----------------------------------------------------------------

$mediaOut = Join-Path $OutputPath 'media'
New-Item -ItemType Directory -Force -Path $mediaOut | Out-Null

# Everything large stays in docs/ and is copied here at build time, so the
# repository holds one copy of each picture. The page offers AVIF and WebP
# variants from site/src/assets first; these are the full-size files behind
# them - the <img> fallbacks, the 1:1 view and the "full size" links.
$mediaFiles = @(
    # Player screenshots.
    'docs/screenshots/current/neural-playback.jpg'
    'docs/screenshots/current/original-comparison.jpg'
    'docs/screenshots/current/recent-videos.jpg'
    'docs/screenshots/current/neural-strength.jpg'
    'docs/screenshots/current/compare-wipe.jpg'
    'docs/screenshots/current/compare-difference.jpg'
    'docs/screenshots/current/photo-wipe.jpg'
    'docs/screenshots/current/saved-comparison-2x2.png'
    'docs/screenshots/current/neural-settings.jpg'
    'docs/screenshots/current/subtitles.jpg'
    'docs/screenshots/current/player-start.jpg'
    # The comparison stills, source and render side by side, unscaled.
    'docs/media/stills/007-first-light-bond.png'
    'docs/media/stills/007-first-light-suit.png'
    'docs/media/stills/resident-evil-requiem-flashlight.png'
    'docs/media/stills/gta6-lucia.png'
    'docs/media/stills/ac-shadows-low-key-limit.png'
    # The demonstration's poster and the link-preview card.
    'docs/media/neural-comparison-poster.jpg'
    'docs/media/social/card-1200x630.jpg'
)
if (-not $SkipMedia) { $mediaFiles += 'docs/media/neural-comparison-demo.mp4' }

foreach ($rel in $mediaFiles) {
    $source = Join-Path $repoRoot $rel
    if (-not (Test-Path $source)) { throw "Media file missing from the repository: $rel" }
    Copy-Item $source (Join-Path $mediaOut (Split-Path $rel -Leaf)) -Force
}

if ($SkipMedia) {
    # test.ps1 asserts every local reference resolves; a placeholder keeps that
    # check honest without copying 7.2 MB on every run.
    Write-TextFile -Path (Join-Path $mediaOut 'neural-comparison-demo.mp4') -Text ''
}

# --- 6. release.json ----------------------------------------------------------

@{
    version     = $release.Version
    tag         = $release.Tag
    releaseUrl  = $release.ReleaseUrl
    publishedAt = $release.PublishedAt
    builtAt     = $tokens.BUILT_AT
    repo        = $repo
    full        = $(if ($release.Full) { @{ name = $release.Full.Name; url = $release.Full.Url; size = $release.Full.Size; sizeText = $release.Full.SizeText } } else { $null })
    core        = $(if ($release.Core) { @{ name = $release.Core.Name; url = $release.Core.Url; size = $release.Core.Size; sizeText = $release.Core.SizeText } } else { $null })
} | ConvertTo-Json -Depth 5 | ForEach-Object { Write-TextFile -Path (Join-Path $OutputPath 'release.json') -Text $_ }

$bytes = (Get-ChildItem $OutputPath -Recurse -File | Measure-Object -Property Length -Sum).Sum
if (-not $Quiet) {
    Write-Host "  built $OutputPath ($(Format-ByteSize $bytes))" -ForegroundColor Green
}
