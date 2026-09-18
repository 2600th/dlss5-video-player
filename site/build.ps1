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
#>
[CmdletBinding()]
param(
    [string]$OutputPath,
    [string]$ReleaseFixture,
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
    ANALYTICS       = $analytics
    BUILT_AT        = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    YEAR            = (Get-Date).ToUniversalTime().Year
}

$html = Expand-Token -Text (Read-TextFile -Path (Join-Path $srcRoot 'index.html')) -Values $tokens

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
# new release has not changed anything worth recrawling for.
$lastMod = if ($publishedIso) { $publishedIso } else { (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd') }
Write-TextFile -Path (Join-Path $OutputPath 'sitemap.xml') -Text @"
<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
  <url>
    <loc>$siteUrl</loc>
    <lastmod>$lastMod</lastmod>
    <changefreq>monthly</changefreq>
    <priority>1.0</priority>
  </url>
</urlset>
"@

# --- 5. media -----------------------------------------------------------------

$mediaOut = Join-Path $OutputPath 'media'
New-Item -ItemType Directory -Force -Path $mediaOut | Out-Null

$mediaFiles = @(
    'docs/screenshots/current/neural-playback.jpg'
    'docs/screenshots/current/neural-strength.jpg'
    'docs/screenshots/current/recent-videos.jpg'
    'docs/screenshots/current/original-comparison.jpg'
    'docs/media/neural-comparison-poster.jpg'
)
if (-not $SkipMedia) { $mediaFiles += 'docs/media/neural-comparison-demo.mp4' }

foreach ($rel in $mediaFiles) {
    $source = Join-Path $repoRoot $rel
    if (-not (Test-Path $source)) { throw "Media file missing from the repository: $rel" }
    Copy-Item $source (Join-Path $mediaOut (Split-Path $rel -Leaf)) -Force
}

if ($SkipMedia) {
    # test.ps1 asserts every local reference resolves; a placeholder keeps that
    # check honest without copying 5.3 MB on every run.
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
