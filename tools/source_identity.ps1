# The commit a package is built from, refused unless it can be traced (P1.30).
#
# Dot-sourced by package_release.ps1, and by tests/PackageSourceIdentity.ps1,
# which runs it against a scratch repository.
#
# A package names the commit it was built from in PACKAGE_MANIFEST.txt, and
# that name is only worth writing if it is true. So:
#
#   * A tree with uncommitted changes to tracked files is refused, always: the
#     commit would not describe what was built. Untracked files are not read -
#     the packager copies an explicit list, and the inputs outside git are held
#     to their locks by hash.
#   * A release is built from the release tag. Two tags were moved after a
#     failed release run (v0.26.0 from a4a3411 to 13f92b7, v0.26.2 from 581a13a
#     to 5dbb3b9), so the tag is checked against HEAD, not assumed.
#   * -Snapshot is the per-push CI package, which is not a release: any
#     committed HEAD, still clean.
function Get-PackageSourceIdentity {
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$ExpectedTag,
        [switch]$Snapshot
    )
    # Windows PowerShell turns a native command's stderr into a terminating
    # error under 'Stop'; the exit codes are read explicitly instead.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $head = & git -C $RepositoryRoot rev-parse --verify HEAD 2>$null
        if ($LASTEXITCODE -ne 0 -or "$head" -notmatch '^[0-9a-f]{40}$') {
            throw "The package's commit cannot be read: '$RepositoryRoot' is not a git checkout with a commit."
        }
        $head = "$head".Trim()
        $changes = @(& git -C $RepositoryRoot status --porcelain --untracked-files=no 2>$null)
        if ($LASTEXITCODE -ne 0) { throw 'git status failed, so the tree cannot be shown to be clean.' }
        $changes = @($changes | Where-Object { "$_".Trim() })
        if ($changes.Count) {
            $listed = ($changes | Select-Object -First 10 | ForEach-Object { "  $_" }) -join "`n"
            throw "The tree has uncommitted changes, so no commit describes this package. Commit or stash them:`n$listed"
        }
        $tagged = & git -C $RepositoryRoot rev-parse -q --verify "refs/tags/$ExpectedTag^{commit}" 2>$null
        $tagged = if ($LASTEXITCODE -eq 0) { "$tagged".Trim() } else { '' }
        if (-not $Snapshot -and $tagged -cne $head) {
            if ($tagged) { throw "Tag $ExpectedTag is $tagged, not HEAD ($head). Build the release from its tag." }
            throw "HEAD ($head) is not tagged $ExpectedTag. Tag the release first, or pass -Snapshot for a package that is not one."
        }
        return [pscustomobject]@{ Commit = $head; Tag = if ($tagged -ceq $head) { $ExpectedTag } else { '' } }
    }
    finally { $ErrorActionPreference = $previous }
}
