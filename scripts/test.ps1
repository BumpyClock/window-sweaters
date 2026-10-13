[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [string]$NativeOutputRoot,
    [string]$NativeIntermediateRoot,
    [switch]$Desktop,
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
if (-not $NoBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Platform $Platform -NativeOutputRoot $NativeOutputRoot -NativeIntermediateRoot $NativeIntermediateRoot -IncludeFixture:$Desktop -IncludeTrackingTests -IncludeContractTests }
$outputRoot = if ($NativeOutputRoot) { [IO.Path]::GetFullPath($NativeOutputRoot).TrimEnd('\') } else { [IO.Path]::GetFullPath((Join-Path $root 'out')).TrimEnd('\') }
$output = Join-Path $outputRoot "$Platform\$Configuration"
$hostExe = Join-Path $output 'WindowSweaters.Host.exe'
if (-not (Test-Path -LiteralPath $hostExe)) { throw "Host artifact not found: $hostExe" }
if ($Platform -eq 'ARM64' -and [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'Arm64') {
    Write-Output 'NOT RUN: ARM64 tests require an ARM64 Windows host. Selected ARM64 Host artifact exists; no ARM64 runtime tests executed.'
    return
}
foreach ($arguments in @('--help', '--target 0', '--target -1', '--target 18446744073709551616',
    '--unknown', '--renderer', '--renderer cpu', '--renderer gpu', '--renderer invalid',
    '--integration-contract --renderer cpu', '--integration-contract --renderer gpu',
    '--test-mode', '--probe-output unused.json', '--target', '--target 1 --target 2',
    '--test-mode --test-mode --target 1', '--integration-contract --test-mode',
    '--storage-dir C:\isolated', '--endpoint-suffix probe',
    '--test-mode --target 1 --storage-dir relative --endpoint-suffix safe',
    '--test-mode --target 1 --storage-dir C:\isolated --endpoint-suffix bad.name',
    '--test-mode --target 1 --storage-dir C:\isolated',
    '--test-mode --target 1 --endpoint-suffix safe')) {
    $process = Start-Process -FilePath $hostExe -WindowStyle Hidden -ArgumentList $arguments -PassThru -Wait
    $expected = if ($arguments -eq '--help') { 0 } else { 2 }
    if ($process.ExitCode -ne $expected) { throw "Argument check '$arguments': expected $expected, got $($process.ExitCode)" }
}
foreach ($arguments in @('--target 1', '--target 1 --test-mode')) {
    $process = Start-Process -FilePath $hostExe -WindowStyle Hidden -ArgumentList $arguments -PassThru -Wait
    if ($process.ExitCode -ne 3) { throw "Fixture target check '$arguments': expected 3, got $($process.ExitCode)" }
}
$contractOutput = Join-Path $output 'HostContract.stdout.log'
$contractError = Join-Path $output 'HostContract.stderr.log'
$contract = Start-Process -FilePath $hostExe -WindowStyle Hidden -ArgumentList '--integration-contract' -PassThru -Wait `
    -RedirectStandardOutput $contractOutput -RedirectStandardError $contractError
if ($contract.ExitCode -ne 0) { throw "Host integration contract failed: $($contract.ExitCode)" }
Get-Content -LiteralPath $contractOutput
$trackingExe = Join-Path $output 'TrackingTests.exe'
if (-not (Test-Path -LiteralPath $trackingExe)) { throw "Tracking test artifact not found: $trackingExe" }
& $trackingExe --native-contract
if ($LASTEXITCODE -ne 0) { throw "Tracking contracts failed: $LASTEXITCODE" }
foreach ($path in @("out\$Platform\$Configuration\WindowSweaters.Configuration.Tests.exe",
    "tests\Applications\build\$Platform\$Configuration\ApplicationsTests.exe",
    "out\$Platform\$Configuration\HostAppearanceTests.exe",
    "tests\IconYarn\build\$Platform\$Configuration\IconYarnTests.exe",
    "out\$Platform\$Configuration\WindowSweaters.ConfigurationHost.Tests.exe",
    "out\$Platform\$Configuration\WindowSweaters.Knitting.Tests.exe",
    "out\RenderQueueTests\$Platform\$Configuration\RenderQueueTests.exe",
    "out\GraphicsRecoveryTests\$Platform\$Configuration\GraphicsRecoveryTests.exe",
    "out\$Platform\$Configuration\VisibilityTests.exe",
    "out\$Platform\$Configuration\HostProbeTests.exe")) {
    $exe = if ($NativeOutputRoot) { Join-Path $output ([IO.Path]::GetFileName($path)) } else { Join-Path $root $path }
    if (-not (Test-Path -LiteralPath $exe)) { throw "Contract artifact not found: $exe" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Contract '$path' failed: $LASTEXITCODE" }
}
Write-Output 'PASS: host CLI/publication, tracking, configuration/session, configuration ownership, knitting, render queue and fixture probe parser contracts; no production host or visible fixture launched.'
if ($Desktop) {
    $fixture = Join-Path $output 'Fixture.exe'
    if (-not (Test-Path -LiteralPath $fixture)) { throw "Fixture artifact not found: $fixture" }
    Write-Output "PREPARED: $fixture"
    Write-Warning 'Desktop verification NOT RUN. Launch Fixture.exe with Computer Use, then use its Start test host and Stop test host controls.'
}
