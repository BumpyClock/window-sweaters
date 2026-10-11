[CmdletBinding()]
param(
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
if (!(Test-Path -LiteralPath $MSBuild)) { throw "MSBuild not found: $MSBuild" }
foreach ($platform in @('x64', 'ARM64')) {
    foreach ($configuration in @('Debug', 'Release')) {
        & $MSBuild (Join-Path $root 'tests\Fixture\Fixture.vcxproj') /nologo /v:minimal /t:VerifyFixtureConfiguration "/p:Platform=$platform" "/p:Configuration=$configuration" "/p:ForceImportAfterCppTargets=$(Join-Path $root 'tests\Fixture\FixtureConfigurationTests.targets')"
        if ($LASTEXITCODE) { throw "Fixture configuration contract failed: $configuration|$platform" }
    }
}
