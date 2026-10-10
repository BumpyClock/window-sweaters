[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Desktop,
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $NoBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -IncludeFixture:$Desktop }
$hostExe = Join-Path $root "out\x64\$Configuration\WindowSweaters.Host.exe"
if (-not (Test-Path -LiteralPath $hostExe)) { throw "Host artifact not found: $hostExe" }
foreach ($arguments in @('--help', '--target 0', '--target -1', '--target 18446744073709551616', '--unknown')) {
    $process = Start-Process -FilePath $hostExe -ArgumentList $arguments -PassThru -Wait
    $expected = if ($arguments -eq '--help') { 0 } else { 2 }
    if ($process.ExitCode -ne $expected) { throw "Argument check '$arguments': expected $expected, got $($process.ExitCode)" }
}
Write-Output 'PASS: host CLI validation; no desktop applications selected.'
if ($Desktop) {
    $fixture = Join-Path $root "out\x64\$Configuration\Fixture.exe"
    if (-not (Test-Path -LiteralPath $fixture)) { throw "Fixture artifact not found: $fixture" }
    Write-Output "PREPARED: $fixture"
    Write-Warning 'Desktop verification NOT RUN. Launch Fixture.exe with Computer Use, then use its Start test host and Stop test host controls.'
}
