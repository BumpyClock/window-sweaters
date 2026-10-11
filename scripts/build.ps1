[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [string]$NativeOutputRoot,
    [string]$NativeIntermediateRoot,
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe',
    [switch]$Rebuild,
    [switch]$IncludeFixture,
    [switch]$IncludeTrackingTests,
    [switch]$IncludeContractTests
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
if (-not (Test-Path -LiteralPath $MSBuild)) { throw "MSBuild not found: $MSBuild" }
$vs = [IO.Path]::GetFullPath((Join-Path (Split-Path $MSBuild -Parent) '..\..\..'))
$tools = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
foreach ($required in @(
    (Join-Path $tools.FullName "bin\Hostx64\$($Platform.ToLowerInvariant())\cl.exe"),
    (Join-Path $tools.FullName "lib\$Platform\libcmt.lib"),
    (Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Lib\10.0.26100.0\um\$Platform\kernel32.lib")
)) {
    if (!(Test-Path -LiteralPath $required)) {
        throw "Missing v145 C++ $Platform build tools or Windows SDK 10.0.26100.0 capability: $required"
    }
}
$properties = @("/p:Configuration=$Configuration", "/p:Platform=$Platform")
$outputRoot = [IO.Path]::GetFullPath((Join-Path $root 'out')).TrimEnd('\')
if ($NativeOutputRoot) {
    $outputRoot = [IO.Path]::GetFullPath($NativeOutputRoot).TrimEnd('\')
    if (!$outputRoot.StartsWith(([IO.Path]::GetFullPath((Join-Path $root 'out')) + '\'), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'NativeOutputRoot must be an isolated directory under repository out.'
    }
    $properties += "/p:NativeOutputRoot=$outputRoot"
    if (!$NativeIntermediateRoot) { $NativeIntermediateRoot = Join-Path $outputRoot 'obj' }
}
if ($NativeIntermediateRoot) {
    $NativeIntermediateRoot = [IO.Path]::GetFullPath($NativeIntermediateRoot).TrimEnd('\')
    if (!$NativeIntermediateRoot.StartsWith(([IO.Path]::GetFullPath((Join-Path $root 'out')) + '\'), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'NativeIntermediateRoot must remain under repository out.'
    }
    $properties += "/p:NativeIntermediateRoot=$NativeIntermediateRoot"
}
$before = & (Join-Path $root 'packaging\Get-SourceInventory.ps1') -Root $root
$output = Join-Path $outputRoot "$Platform\$Configuration"
$receipt = Join-Path $output 'native-build-report.json'
if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt }
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$project = Join-Path $root 'src\Host\WindowSweaters.Host.vcxproj'
& $MSBuild $project /nologo /m /verbosity:minimal "/t:$target" @properties
if ($LASTEXITCODE -ne 0) { throw "Host build failed: $LASTEXITCODE" }
$settings = Join-Path $root 'src\Settings\WindowSweaters.Settings.vcxproj'
& $MSBuild $settings /nologo /m /verbosity:minimal "/t:$target" @properties
if ($LASTEXITCODE -ne 0) { throw "Settings build failed: $LASTEXITCODE" }
if ($IncludeFixture) {
    $fixture = Join-Path $root 'tests\Fixture\Fixture.vcxproj'
    if (-not (Test-Path -LiteralPath $fixture)) { throw "Fixture project not found: $fixture" }
    & $MSBuild $fixture /nologo /m /verbosity:minimal "/t:$target" @properties
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
        & $MSBuild (Join-Path $root $path) /nologo /m /verbosity:minimal "/t:$target" @properties
        if ($LASTEXITCODE -ne 0) { throw "Contract project '$path' build failed: $LASTEXITCODE" }
    }
}
if ($IncludeTrackingTests) {
    $tracking = Join-Path $root 'tests\Tracking\TrackingTests.vcxproj'
    if (-not (Test-Path -LiteralPath $tracking)) { throw "Tracking test project not found: $tracking" }
    & $MSBuild $tracking /nologo /m /verbosity:minimal "/t:$target" @properties
    if ($LASTEXITCODE -ne 0) { throw "Tracking test build failed: $LASTEXITCODE" }
}
$after = & (Join-Path $root 'packaging\Get-SourceInventory.ps1') -Root $root
if (($before | ConvertTo-Json -Depth 4 -Compress) -ne ($after | ConvertTo-Json -Depth 4 -Compress)) {
    throw 'Source changed during native build. Rebuild after all source writers are frozen.'
}
$files = @(Get-ChildItem -LiteralPath $output -Recurse -File |
    Where-Object Extension -in @('.exe', '.dll', '.pri') |
    Sort-Object FullName | ForEach-Object {
        [ordered]@{ Path = $_.FullName.Substring($output.Length + 1); SHA256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
    })
[ordered]@{
    SchemaVersion = 1; Platform = $Platform; Configuration = $Configuration
    NativeOutputRoot = $outputRoot.Substring($root.Length + 1)
    SourceFiles = $after.Files; Outputs = $files
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receipt -Encoding UTF8
Write-Output (Join-Path $output 'WindowSweaters.Host.exe')
