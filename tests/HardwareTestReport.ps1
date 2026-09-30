# P1.31: the complete package needs the hardware suite's JUnit report, from
# this build, with every required test run and passed.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File HardwareTestReport.ps1 -Tools <repo>\tools
#
# Synthetic reports in the shape ctest --output-junit writes.
param([Parameter(Mandatory)][string]$Tools)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $Tools 'hardware_report.ps1')

$failures = New-Object Collections.Generic.List[string]
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("HardwareTestReport-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$binary = Join-Path $scratch 'DLSSVideoPlayer.exe'
$report = Join-Path $scratch 'hardware-junit.xml'

function Write-Report {
    param([hashtable]$Status)
    $cases = foreach ($name in $Status.Keys) {
        $body = if ($Status[$name] -eq 'fail') { '<failure message="Failed"/>' } else { '' }
        "  <testcase name=`"$name`" classname=`"$name`" time=`"1`" status=`"$($Status[$name])`">$body</testcase>"
    }
    $text = "<?xml version=`"1.0`" encoding=`"UTF-8`"?>`n<testsuite name=`"Win32-MSBuild`" tests=`"$($Status.Count)`">`n$($cases -join "`n")`n</testsuite>`n"
    [IO.File]::WriteAllText($report, $text)
}
function All-Run {
    $status = @{}
    foreach ($name in $script:RequiredHardwareTests) { $status[$name] = 'run' }
    $status['DlssgProbeSmoke'] = 'run'
    return $status
}
function Expect {
    param([string]$Name, [string]$Says)
    try {
        $count = Assert-HardwareTestReport -Report $report -Binaries @($binary)
        if ($Says) { $failures.Add("${Name}: accepted ($count tests), expected a refusal") }
    }
    catch {
        if (-not $Says) { $failures.Add("${Name}: refused with '$_'") }
        elseif ("$_" -notlike "*$Says*") { $failures.Add("${Name}: refused with '$_', expected '*$Says*'") }
    }
}

try {
    Set-Content -LiteralPath $binary -Value 'build'
    Expect 'no report' 'No hardware test report'
    Start-Sleep -Milliseconds 20
    Write-Report (All-Run)
    Expect 'a passing report' ''

    $failed = All-Run; $failed['ExportMatrixSmoke'] = 'fail'
    Write-Report $failed
    Expect 'a failed test' 'ExportMatrixSmoke (fail)'

    # A skipped smoke is how a machine without the GPU goes green.
    $skipped = All-Run; $skipped['NeuralRangeRenderSmoke'] = 'notrun'
    Write-Report $skipped
    Expect 'a skipped test' 'NeuralRangeRenderSmoke (notrun)'

    $missing = All-Run; $missing.Remove('AudioClockSmoke')
    Write-Report $missing
    Expect 'a missing test' 'does not include AudioClockSmoke'

    Write-Report (All-Run)
    (Get-Item -LiteralPath $binary).LastWriteTimeUtc = (Get-Item -LiteralPath $report).LastWriteTimeUtc.AddMinutes(1)
    Expect 'a report older than the build' 'predates'
}
finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAILED: $_" }
    exit 1
}
Write-Host 'Hardware test report: the complete package gate holds.'
exit 0
