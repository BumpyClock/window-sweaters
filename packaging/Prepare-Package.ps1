[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$MSBuild,
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Publisher,
    [Parameter(Mandatory)][string]$PublisherDisplayName,
    [Parameter(Mandatory)][string]$DisplayName,
    [Parameter(Mandatory)][string]$Version,
    [string]$StoreIdentityProps,
    [ValidateSet('Development', 'Store')][string]$Channel = 'Development'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
$stagePath = [IO.Path]::GetFullPath($Stage)
$ownedRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'obj')) + '\'
if (!$stagePath.StartsWith($ownedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package staging must stay inside packaging\obj.'
}
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$' -or @($Version.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) {
    throw 'PackageVersion must contain four integers between 0 and 65535.'
}
if ($Channel -eq 'Store' -and ($Name + $Publisher + $PublisherDisplayName + $DisplayName) -match '(?i)development|REPLACE_WITH') {
    throw 'Store mode rejects development and example identity values.'
}
if ($Channel -eq 'Store') {
    if (!$StoreIdentityProps -or !(Test-Path -LiteralPath $StoreIdentityProps -PathType Leaf)) {
        throw 'Store mode requires all identity values in a local MSBuild user property file.'
    }
    [xml]$storeProps = Get-Content -LiteralPath $StoreIdentityProps
    $storeNs = New-Object Xml.XmlNamespaceManager($storeProps.NameTable)
    $storeNs.AddNamespace('m', 'http://schemas.microsoft.com/developer/msbuild/2003')
    foreach ($field in @(
        @{ Key = 'PackageName'; Value = $Name },
        @{ Key = 'PackagePublisher'; Value = $Publisher },
        @{ Key = 'PackagePublisherDisplayName'; Value = $PublisherDisplayName }
    )) {
        $nodes = @($storeProps.SelectNodes("/m:Project/m:PropertyGroup/m:$($field.Key)", $storeNs))
        if ($nodes.Count -ne 1 -or [string]::IsNullOrWhiteSpace($nodes[0].InnerText) -or $nodes[0].InnerText -ne $field.Value) {
            throw "Store identity must be specified once, literally in StoreIdentityProps, without CLI/environment overrides: $($field.Key)"
        }
    }
}
$output = Join-Path $root 'out\x64\Release'
$vs = [IO.Path]::GetFullPath((Join-Path (Split-Path $MSBuild -Parent) '..\..\..'))
$tools = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$dumpbin = Join-Path $tools.FullName 'bin\Hostx64\x64\dumpbin.exe'
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64'
foreach ($path in @($dumpbin, (Join-Path $sdk 'mt.exe'), (Join-Path $sdk 'makeappx.exe'))) {
    if (!(Test-Path -LiteralPath $path)) { throw "Missing packaging tool: $path" }
}
if (Test-Path -LiteralPath $stagePath) { Remove-Item -LiteralPath $stagePath -Recurse -Force }
[void][IO.Directory]::CreateDirectory($stagePath)
$intermediate = Split-Path $stagePath -Parent
$runtimeList = Join-Path $intermediate 'runtime-files.txt'
& $MSBuild (Join-Path $root 'src\Settings\WindowSweaters.Settings.vcxproj') /nologo /v:q /t:CollectWindowSweatersRuntime /p:Configuration=Release /p:Platform=x64 "/p:ForceImportAfterCppTargets=$(Join-Path $PSScriptRoot 'CollectRuntime.targets')" "/p:PackageRuntimeList=$runtimeList"
if ($LASTEXITCODE -ne 0) { throw 'Could not collect the Settings project runtime items.' }

$payload = @{}
$applicationInputs = @{}
function Add-Payload([string]$source, [string]$relative, [string]$origin) {
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|\\)\.\.(\\|$)' -or $relative -match '(?i)\.(pdb|ilk|lib|log|user|pfx|pem|key)$') {
        throw "Unsafe package path: $relative"
    }
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing required $origin payload: $relative" }
    $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ($payload.ContainsKey($relative)) {
        if ($payload[$relative].SHA256 -ne $hash) { throw "Conflicting package payload: $relative" }
        return
    }
    $destination = Join-Path $stagePath $relative
    [void][IO.Directory]::CreateDirectory((Split-Path $destination -Parent))
    Copy-Item -LiteralPath $source -Destination $destination
    $payload[$relative] = [ordered]@{ Path = $relative; Origin = $origin; SHA256 = $hash }
}
foreach ($exe in @('WindowSweaters.Settings.exe', 'WindowSweaters.Host.exe')) {
    Add-Payload (Join-Path $output $exe) $exe 'application'
    $applicationInputs[$exe] = $payload[$exe].SHA256
    & (Join-Path $PSScriptRoot 'Remove-PrivateDebugPaths.ps1') -Path (Join-Path $stagePath $exe)
    $payload[$exe].SHA256 = (Get-FileHash -LiteralPath (Join-Path $stagePath $exe)).Hash
    $payload[$exe].Origin = 'application; staged CodeView PDB path normalized to basename'
    $embedded = Join-Path $intermediate "$exe.manifest.xml"
    & (Join-Path $sdk 'mt.exe') -nologo "-inputresource:$(Join-Path $stagePath $exe);#1" "-out:$embedded" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect embedded manifest: $exe" }
    [xml]$nativeManifest = Get-Content -LiteralPath $embedded
    $dpi = $nativeManifest.SelectSingleNode("//*[local-name()='dpiAwareness']")
    $level = $nativeManifest.SelectSingleNode("//*[local-name()='requestedExecutionLevel']")
    if (!$dpi -or $dpi.InnerText -ne 'PerMonitorV2' -or !$level -or $level.level -ne 'asInvoker' -or $level.uiAccess -ne 'false') {
        throw "Native manifest must retain PMv2, asInvoker and uiAccess=false: $exe"
    }
}
$runtimeRows = @(Get-Content -LiteralPath $runtimeList | Where-Object { $_ })
if ($runtimeRows.Count -lt 10) { throw 'Self-contained runtime item list is unexpectedly empty.' }
foreach ($line in $runtimeRows) {
    $source, $relative = $line.Split('|', 2)
    if ([IO.Path]::GetExtension($relative) -eq '.winmd') { continue }
    $built = Join-Path $output $relative
    if (!(Test-Path -LiteralPath $source -PathType Leaf) -or !(Test-Path -LiteralPath $built -PathType Leaf)) {
        throw "Missing project runtime output: $relative. Rebuild Settings Release|x64."
    }
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $built).Hash) {
        throw "Runtime output differs from the pinned dependency: $relative. Rebuild Settings."
    }
    Add-Payload $built $relative 'WindowsAppSDK project runtime item'
}
foreach ($required in @('Microsoft.ui.xaml.dll', 'Microsoft.UI.Xaml.Controls.pri', 'Microsoft.WindowsAppRuntime.dll', 'Microsoft.WindowsAppRuntime.pri', 'Microsoft.WindowsAppRuntime.Bootstrap.dll')) {
    if (!$payload.ContainsKey($required)) { throw "Missing self-contained runtime: $required" }
}
$licenseRoots = @($runtimeRows | ForEach-Object { $_.Split('|', 2)[0] } |
    Where-Object { $_ -match '\\runtimes(-framework)?\\' } |
    ForEach-Object { $_ -replace '\\runtimes(-framework)?\\.*$', '' } | Sort-Object -Unique)
$appSDKRoot = Join-Path $env:USERPROFILE '.nuget\packages\microsoft.windowsappsdk\2.5.1'
$localAppSDKRoot = Join-Path $root 'src\Settings\.packages\Microsoft.WindowsAppSDK.2.5.1'
if (Test-Path $localAppSDKRoot) { $appSDKRoot = $localAppSDKRoot }
$licenseRoots += $appSDKRoot
foreach ($licenseRoot in $licenseRoots) {
    foreach ($licenseName in @('license.txt', 'NOTICE.txt', 'ThirdPartyNotices.txt', 'ThirdPartyNotices.md')) {
        $licensePath = Join-Path $licenseRoot $licenseName
        if (Test-Path -LiteralPath $licensePath -PathType Leaf) {
            $packageId = (Split-Path (Split-Path $licenseRoot -Parent) -Leaf) + '.' + (Split-Path $licenseRoot -Leaf)
            Add-Payload $licensePath "Licenses\$packageId\$licenseName" 'upstream runtime license/notice'
        }
    }
}
$redist = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.' } | Sort-Object Name -Descending | Select-Object -First 1
$crt = Join-Path $redist.FullName 'x64\Microsoft.VC145.CRT'
foreach ($file in Get-ChildItem -LiteralPath $crt -Filter '*.dll') {
    Add-Payload $file.FullName $file.Name 'Visual C++ v145 app-local redistributable'
}
Add-Payload (Join-Path $vs 'Licenses\1033\Redist.txt') 'Licenses\VisualCpp\Redist.txt' 'Visual Studio redistributable list'

$dependencies = @()
foreach ($relative in @($payload.Keys | Where-Object { $_ -match '(?i)\.(dll|exe)$' } | Sort-Object)) {
    $binary = Join-Path $stagePath $relative
    $headers = & $dumpbin /nologo /headers $binary
    if ($LASTEXITCODE -ne 0 -or !($headers -match '8664 machine \(x64\)')) { throw "Non-x64 package binary: $relative" }
    $imports = & $dumpbin /nologo /dependents $binary
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect PE dependencies: $relative" }
    $names = @($imports | Where-Object { $_ -match '^\s+[\w.\-]+\.dll\s*$' } | ForEach-Object { $_.Trim() } | Sort-Object -Unique)
    foreach ($dependency in $names) {
        $kind = 'unresolved'
        if ($payload.ContainsKey($dependency)) { $kind = 'package' }
        elseif ($dependency -match '^(api-ms-|ext-ms-)') { $kind = 'Windows API set' }
        elseif (Test-Path -LiteralPath (Join-Path "$env:windir\System32" $dependency)) { $kind = 'Windows system DLL (build machine)' }
        $dependencies += [ordered]@{ Binary = $relative; Dependency = $dependency; Resolution = $kind }
        if ($kind -eq 'unresolved') { throw "Unresolved dependency: $relative -> $dependency" }
        if ($relative -eq 'WindowSweaters.Host.exe' -and $dependency -match '^(?i)(msvcp|vcruntime|concrt|Microsoft\.UI|Microsoft\.WindowsAppRuntime)') {
            throw "Host must retain its static CRT and no WinUI dependency: $dependency"
        }
    }
}
& (Join-Path $PSScriptRoot 'Generate-Assets.ps1') -OutputDirectory (Join-Path $PSScriptRoot 'Assets')
foreach ($asset in Get-ChildItem (Join-Path $PSScriptRoot 'Assets') -Filter '*.png') {
    Add-Payload $asset.FullName "Assets\$($asset.Name)" 'original yarn artwork'
}
Add-Payload (Join-Path $root 'LICENSE') 'LICENSE' 'GPL-3.0'
Add-Payload (Join-Path $root 'NOTICE.md') 'NOTICE.md' 'upstream attribution'

$artifactDirectory = Join-Path $PSScriptRoot "artifacts\$Channel"
[void][IO.Directory]::CreateDirectory($artifactDirectory)
$sourceName = "WindowSweaters_${Version}_${Channel}_Source.zip"
$sourceZip = Join-Path $artifactDirectory $sourceName
$sourceInventory = & (Join-Path $PSScriptRoot 'New-SourceArchive.ps1') -Root $root -Destination $sourceZip
$sourceFiles = @($sourceInventory.Files.Path)
$sourceHash = (Get-FileHash -LiteralPath $sourceZip).Hash
$sourceGuidance = @"
Window Sweaters is licensed under GPL version 3. See LICENSE and NOTICE.md.
The complete repository source snapshot for this build accompanies this artifact:
$sourceName
SHA256: $sourceHash
Preserve and provide that archive when redistributing this package. It includes
application and packaging sources, tests, build scripts and pinned package
references; dependencies must be restored from their declared upstream sources.
packaging\SourceInventory.json allows source enumeration without Git metadata.
Do not substitute a later branch checkout for this exact source snapshot.
Public project: https://github.com/BumpyClock/window-sweaters
This unsigned artifact is not a Store submission or publication.
The distributor must review GPL and Microsoft Store terms before distribution.
Microsoft Windows App SDK and Visual C++ runtime files retain their own licenses.
"@
[IO.File]::WriteAllText((Join-Path $stagePath 'SOURCE.txt'), $sourceGuidance)

[xml]$manifest = Get-Content (Join-Path $PSScriptRoot 'Package.appxmanifest')
$manifest.Package.Identity.Name = $Name
$manifest.Package.Identity.Publisher = $Publisher
$manifest.Package.Identity.Version = $Version
$manifest.Package.Properties.DisplayName = $DisplayName
$manifest.Package.Properties.PublisherDisplayName = $PublisherDisplayName
$applications = @($manifest.Package.Applications.Application)
$applications[0].VisualElements.DisplayName = $DisplayName
$applications[1].VisualElements.DisplayName = "$DisplayName background host"
$applications[1].Extensions.Extension.StartupTask.DisplayName = $DisplayName
$manifest.Save((Join-Path $stagePath 'AppxManifest.xml'))
foreach ($exe in $applicationInputs.Keys) {
    if ($applicationInputs[$exe] -ne (Get-FileHash -LiteralPath (Join-Path $output $exe)).Hash) {
        throw "Native output changed during packaging: $exe. Wait for integration builds to finish, then package the stable prebuilt outputs."
    }
}
$report = [ordered]@{
    Channel = $Channel; Name = $Name; Version = $Version
    SourceArchive = $sourceName; SourceSHA256 = $sourceHash; SourceFileCount = $sourceFiles.Count
    Runtime = 'Host static native CRT; Settings WindowsAppSDK 2.5.1 self-contained plus app-local v145 CRT; Windows 11 x64 APIs remain OS dependencies.'
    ApplicationInputSHA256 = $applicationInputs
    Payload = @($payload.Values | Sort-Object { $_.Path })
    Dependencies = $dependencies
    Limitations = @('Unsigned; no installation or launch', 'OS imports inspected on build machine, not a clean Windows 11 22000 machine', 'No Store certification, upload or publication')
}
$report | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $artifactDirectory 'payload-report.json') -Encoding UTF8
Write-Host "Prepared $($payload.Count) allowlisted payload files; corresponding source: $sourceName"
