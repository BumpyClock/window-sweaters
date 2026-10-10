[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Desktop,
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $NoBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -IncludeFixture:$Desktop -IncludeTrackingTests }
$hostExe = Join-Path $root "out\x64\$Configuration\WindowSweaters.Host.exe"
if (-not (Test-Path -LiteralPath $hostExe)) { throw "Host artifact not found: $hostExe" }
foreach ($arguments in @('--help', '--target 0', '--target -1', '--target 18446744073709551616',
    '--unknown', '--test-mode', '--probe-output unused.json', '--target', '--target 1 --target 2',
    '--test-mode --test-mode --target 1', '--integration-contract --test-mode')) {
    $process = Start-Process -FilePath $hostExe -ArgumentList $arguments -PassThru -Wait
    $expected = if ($arguments -eq '--help') { 0 } else { 2 }
    if ($process.ExitCode -ne $expected) { throw "Argument check '$arguments': expected $expected, got $($process.ExitCode)" }
}
foreach ($arguments in @('--target 1', '--target 1 --test-mode')) {
    $process = Start-Process -FilePath $hostExe -ArgumentList $arguments -PassThru -Wait
    if ($process.ExitCode -ne 3) { throw "Fixture target check '$arguments': expected 3, got $($process.ExitCode)" }
}
$contractOutput = Join-Path $root "out\x64\$Configuration\HostContract.stdout.log"
$contractError = Join-Path $root "out\x64\$Configuration\HostContract.stderr.log"
$contract = Start-Process -FilePath $hostExe -ArgumentList '--integration-contract' -PassThru -Wait `
    -RedirectStandardOutput $contractOutput -RedirectStandardError $contractError
if ($contract.ExitCode -ne 0) { throw "Host integration contract failed: $($contract.ExitCode)" }
Get-Content -LiteralPath $contractOutput
$trackingExe = Join-Path $root "out\x64\$Configuration\TrackingTests.exe"
if (-not (Test-Path -LiteralPath $trackingExe)) { throw "Tracking test artifact not found: $trackingExe" }
& $trackingExe --native-contract
if ($LASTEXITCODE -ne 0) { throw "Tracking contracts failed: $LASTEXITCODE" }
Write-Output 'PASS: host CLI, action/geometry and tracking contracts; no production host or desktop fixture launched.'
if ($Desktop) {
    $fixture = Join-Path $root "out\x64\$Configuration\Fixture.exe"
    if (-not (Test-Path -LiteralPath $fixture)) { throw "Fixture artifact not found: $fixture" }
    Write-Output "PREPARED: $fixture"
    Write-Warning 'Desktop verification NOT RUN. Launch Fixture.exe with Computer Use, then use its Start test host and Stop test host controls.'
}
