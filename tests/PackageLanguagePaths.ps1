# A translation may ship as lang\<tag>.lang, pt-BR's included, while the
# legacy pt-BR pack the first builds carried stays forbidden.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File PackageLanguagePaths.ps1 -Tools <repo>\tools
#
# verify_package.ps1 allowed lang/pt-BR.lang by name and then threw for it as
# a Portuguese artifact. Its Test-LegacyLanguageArtifact is read out of the
# script itself, which runs a package check when it is invoked.
param([Parameter(Mandatory)][string]$Tools)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script = Join-Path $Tools 'verify_package.ps1'
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw "verify_package.ps1 does not parse: $($errors[0])" }
$function = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-LegacyLanguageArtifact'
}, $true)
if (-not $function) { throw 'verify_package.ps1 has no Test-LegacyLanguageArtifact' }
. ([scriptblock]::Create($function.Extent.Text))

$failures = New-Object Collections.Generic.List[string]
foreach ($allowed in @('lang/pt-BR.lang', 'lang/zh-Hans.lang', 'lang/de.lang', 'lang/pt-PT.lang', 'DLSSVideoPlayer.exe', 'docs/USAGE.md')) {
    if (Test-LegacyLanguageArtifact -RelativePath $allowed) { $failures.Add("${allowed}: refused, but is a translation or not a language file") }
}
foreach ($forbidden in @('pt-BR.lang', 'languages/pt-BR.lang', 'languages/de.lang', 'lang/pt-BR/strings.lang',
                         'lang/pt-BR.lang.bak', 'docs/USAGE.pt-br.md', 'LANG/pt-BR.lang', 'neural-runtime/pt-br.dll')) {
    if (-not (Test-LegacyLanguageArtifact -RelativePath $forbidden)) { $failures.Add("${forbidden}: allowed, but is a legacy language artifact") }
}

if ($failures.Count -ne 0) {
    $failures | ForEach-Object { Write-Host "FAIL $_" }
    exit 1
}
Write-Host 'PackageLanguagePaths: passed'
