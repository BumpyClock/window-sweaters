[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe',
    [switch]$Rebuild,
    [switch]$IncludeFixture
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path -LiteralPath $MSBuild)) { throw "MSBuild not found: $MSBuild" }
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$project = Join-Path $root 'src\Host\WindowSweaters.Host.vcxproj'
& $MSBuild $project /nologo /m /verbosity:minimal "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "Host build failed: $LASTEXITCODE" }
if ($IncludeFixture) {
    $fixture = Join-Path $root 'tests\Fixture\Fixture.vcxproj'
    if (-not (Test-Path -LiteralPath $fixture)) { throw "Fixture project not found: $fixture" }
    & $MSBuild $fixture /nologo /m /verbosity:minimal "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "Fixture build failed: $LASTEXITCODE" }
}
Write-Output (Join-Path $root "out\x64\$Configuration\WindowSweaters.Host.exe")
