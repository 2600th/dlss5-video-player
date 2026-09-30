# The complete package ships only a build the GPU and audio suites passed on
# (P1.31).
#
# Dot-sourced by package_release.ps1, and by tests/HardwareTestReport.ps1.
#
# CI never runs those suites: gpu-tests.yml waits on a self-hosted GPU runner
# that does not exist, and build.yml and release.yml run the portable preset,
# which excludes gpu|audio. Neural renders, frame generation, direct NVENC,
# the export matrix and the audio clock therefore had no assertion anywhere
# before a tag shipped. The complete zip is assembled on the maintainer's
# machine, which has the GPU, so that is where the gate goes: it needs the
# JUnit report of the hardware preset, run on this very build, with every
# test run and passed.
#
#   ctest --preset hardware --output-junit hardware-junit.xml
#
# writes it to <build directory>/hardware-junit.xml.

# The tests a report must show as run and passed. A skipped one (exit 125:
# no runtime, no GPU, no FFmpeg) is not a pass, because a skip is exactly how
# a machine without the hardware would have produced a green report.
$script:RequiredHardwareTests = @(
    'NeuralRangeRenderSmoke', 'NeuralDirectEncodeSmoke', 'NvencDirectIdentitySmoke',
    'ExportMatrixSmoke', 'FrameGenerationSmoke', 'AudioClockSmoke', 'MediaGpuSmoke',
    'UpscalingGpuSmoke'
)

function Assert-HardwareTestReport {
    param(
        [Parameter(Mandatory)][string]$Report,
        # The binaries the report must be newer than: the build it tested.
        [Parameter(Mandatory)][string[]]$Binaries
    )
    $command = 'ctest --preset hardware --output-junit hardware-junit.xml'
    if (-not (Test-Path -LiteralPath $Report -PathType Leaf)) {
        throw "No hardware test report at '$Report'. The complete package ships only a build the GPU and audio suites passed on; run: $command"
    }
    $written = (Get-Item -LiteralPath $Report).LastWriteTimeUtc
    foreach ($binary in $Binaries) {
        if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Built binary is missing: $binary" }
        if ((Get-Item -LiteralPath $binary).LastWriteTimeUtc -gt $written) {
            throw "The hardware test report predates $(Split-Path -Leaf $binary), so it tested another build. Run again: $command"
        }
    }
    try { [xml]$xml = Get-Content -LiteralPath $Report -Raw }
    catch { throw "The hardware test report is not readable XML: $_" }
    $suite = $xml.testsuite
    if (-not $suite) { throw 'The hardware test report has no <testsuite>.' }
    $cases = @($suite.testcase)
    if (-not $cases.Count) { throw 'The hardware test report ran no tests.' }
    $failed = @($cases | Where-Object { [string]$_.status -ne 'run' -or $_.SelectSingleNode('failure') })
    if ($failed.Count) {
        $names = ($failed | ForEach-Object { "$($_.name) ($($_.status))" }) -join ', '
        throw "The hardware suite did not pass on this build: $names."
    }
    $ran = @($cases | ForEach-Object { [string]$_.name })
    $missing = @($script:RequiredHardwareTests | Where-Object { $ran -cnotcontains $_ })
    if ($missing.Count) { throw "The hardware test report does not include $($missing -join ', ')." }
    return $cases.Count
}
