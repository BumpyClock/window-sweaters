[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe',
    [switch]$Rebuild,
    [switch]$IncludeFixture,
    [switch]$IncludeTrackingTests,
    [switch]$IncludeContractTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path -LiteralPath $MSBuild)) { throw "MSBuild not found: $MSBuild" }
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$project = Join-Path $root 'src\Host\WindowSweaters.Host.vcxproj'
& $MSBuild $project /nologo /m /verbosity:minimal "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "Host build failed: $LASTEXITCODE" }
$settings = Join-Path $root 'src\Settings\WindowSweaters.Settings.vcxproj'
& $MSBuild $settings /nologo /m /verbosity:minimal "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "Settings build failed: $LASTEXITCODE" }
if ($IncludeFixture) {
    $fixture = Join-Path $root 'tests\Fixture\Fixture.vcxproj'
    if (-not (Test-Path -LiteralPath $fixture)) { throw "Fixture project not found: $fixture" }
    & $MSBuild $fixture /nologo /m /verbosity:minimal "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "Fixture build failed: $LASTEXITCODE" }
}
if ($IncludeContractTests) {
    foreach ($path in @('tests\Configuration\WindowSweaters.Configuration.Tests.vcxproj',
        'tests\Applications\ApplicationsTests.vcxproj',
        'tests\Integration\ConfigurationHostTests.vcxproj',
        'src\Host\HostAppearanceTests.vcxproj',
        'tests\IconYarn\IconYarnTests.vcxproj',
        'tests\Knitting\WindowSweaters.Knitting.Tests.vcxproj',
        'tests\Rendering\RenderQueueTests.vcxproj',
        'tests\Rendering\GraphicsRecoveryTests.vcxproj',
        'tests\Visibility\VisibilityTests.vcxproj',
        'tests\Fixture\HostProbeTests.vcxproj')) {
        & $MSBuild (Join-Path $root $path) /nologo /m /verbosity:minimal "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64
        if ($LASTEXITCODE -ne 0) { throw "Contract project '$path' build failed: $LASTEXITCODE" }
    }
}
if ($IncludeTrackingTests) {
    $tracking = Join-Path $root 'tests\Tracking\TrackingTests.vcxproj'
    if (-not (Test-Path -LiteralPath $tracking)) { throw "Tracking test project not found: $tracking" }
    & $MSBuild $tracking /nologo /m /verbosity:minimal "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "Tracking test build failed: $LASTEXITCODE" }
}
Write-Output (Join-Path $root "out\x64\$Configuration\WindowSweaters.Host.exe")
