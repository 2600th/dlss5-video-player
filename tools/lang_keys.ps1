<#
.SYNOPSIS
Lists what a translation is missing against the English strings.

.DESCRIPTION
Writes the English template with DLSSVideoPlayer.exe --lang-template and
compares its keys with lang\<Language>.lang:

- every English key the file lacks, printed as its template line
  (key = English text), ready to translate and paste in;
- every key the file has that English does not, to delete.

Exits 1 when either list is not empty. Placeholders and parse problems are
PolicyTests' to check (shipped_translations_are_complete_test); this is the
list to work from after Localization.h gains or loses keys.

.EXAMPLE
tools\lang_keys.ps1 zh-Hans
tools\lang_keys.ps1 zh-Hans -Player build-upscaling\Release\DLSSVideoPlayer.exe
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Language,
    [string]$Player
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if (-not $Player) { $Player = Join-Path $repositoryRoot 'build-ci\Release\DLSSVideoPlayer.exe' }
if (-not (Test-Path -LiteralPath $Player -PathType Leaf)) { throw "No player at '$Player'. Build it (cmake --build --preset ci) or pass -Player." }
$file = Join-Path $repositoryRoot "lang\$Language.lang"
if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "No translation at '$file'." }

$template = Join-Path ([IO.Path]::GetTempPath()) "dlss5-lang-template-$PID.lang"
try {
    $process = Start-Process -FilePath $Player -ArgumentList @('--lang-template', "`"$template`"") -Wait -PassThru -NoNewWindow
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $template)) { throw "--lang-template failed (exit $($process.ExitCode))." }
    $utf8 = New-Object Text.UTF8Encoding($false)
    # key -> line, for every `key = text` line; '#' lines are comments.
    function Read-Lines([string]$Path) {
        $lines = [ordered]@{}
        foreach ($line in [IO.File]::ReadAllLines($Path, $utf8)) {
            $trimmed = $line.Trim()
            if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) { continue }
            $equals = $trimmed.IndexOf('=')
            if ($equals -lt 1) { continue }
            $key = $trimmed.Substring(0, $equals).Trim()
            if (-not $lines.Contains($key)) { $lines[$key] = $trimmed }
        }
        return $lines
    }
    $english = Read-Lines $template
    $translation = Read-Lines $file
    $missing = @($english.Keys | Where-Object { -not $translation.Contains($_) })
    $extra = @($translation.Keys | Where-Object { -not $english.Contains($_) })
    Write-Host "$Language.lang: $($translation.Count) keys; English has $($english.Count). Missing $($missing.Count), not in English $($extra.Count)."
    if ($missing.Count) {
        Write-Host ''
        Write-Host '# Missing (English text, to translate):'
        foreach ($key in $missing) { [Console]::Out.WriteLine($english[$key]) }
    }
    if ($extra.Count) {
        Write-Host ''
        Write-Host '# Not English keys (delete):'
        foreach ($key in $extra) { [Console]::Out.WriteLine($key) }
    }
    if ($missing.Count -or $extra.Count) { exit 1 }
}
finally {
    Remove-Item -LiteralPath $template -ErrorAction SilentlyContinue
}
