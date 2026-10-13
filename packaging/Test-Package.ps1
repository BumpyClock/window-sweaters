[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArtifactDirectory,
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Publisher,
    [Parameter(Mandatory)][string]$PublisherDisplayName,
    [Parameter(Mandatory)][string]$Version,
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [string]$NativeOutputRoot
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($NativeOutputRoot) { $NativeOutputRoot = [IO.Path]::GetFullPath($NativeOutputRoot).TrimEnd('\') }
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$' -or @($Version.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) {
    throw 'PackageVersion must contain four integers between 0 and 65535.'
}
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64'
$artifactRoot = [IO.Path]::GetFullPath($ArtifactDirectory).TrimEnd('\')
if (!$artifactRoot.StartsWith(([IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'artifacts')) + '\'), [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($artifactRoot) -ne $Platform) {
    throw 'Package validation must use the matching architecture directory under packaging\artifacts.'
}
$versionRoot = Split-Path $artifactRoot -Parent
$channel = [IO.Path]::GetFileName((Split-Path $versionRoot -Parent))
$expectedArtifact = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "artifacts\$channel\$Version\$Platform")).TrimEnd('\')
if ($artifactRoot -ne $expectedArtifact) { throw 'Package validation must use the selected channel/version/platform artifact directory.' }
$buildReceipt = & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -Platform $Platform -NativeOutputRoot $NativeOutputRoot
$packages = @(Get-ChildItem -LiteralPath $ArtifactDirectory -Recurse -Filter '*.msix' |
    Where-Object { $_.FullName -notmatch '\\Dependencies\\' })
if ($packages.Count -ne 1) { throw "Expected exactly one $Platform .msix artifact; found $($packages.Count). Move previous artifacts before rebuilding." }
$package = $packages[0]
$unpacked = Join-Path $PSScriptRoot "obj\$channel\$Version\$Platform\inspection"
if (Test-Path -LiteralPath $unpacked) { Remove-Item -LiteralPath $unpacked -Recurse -Force }
& (Join-Path $sdk 'makeappx.exe') unpack /p $package.FullName /d $unpacked /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'MakeAppx validated unpack failed.' }
[xml]$manifest = Get-Content (Join-Path $unpacked 'AppxManifest.xml')
$ns = New-Object Xml.XmlNamespaceManager($manifest.NameTable)
$ns.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
$ns.AddNamespace('uap', 'http://schemas.microsoft.com/appx/manifest/uap/windows10')
$ns.AddNamespace('uap10', 'http://schemas.microsoft.com/appx/manifest/uap/windows10/10')
$ns.AddNamespace('desktop', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10')
function Assert-Package([bool]$condition, [string]$message) { if (!$condition) { throw $message } }
$identity = $manifest.Package.Identity
Assert-Package ($identity.Name -eq $Name -and $identity.Publisher -eq $Publisher -and $identity.Version -eq $Version -and $identity.ProcessorArchitecture -eq $Platform.ToLowerInvariant()) 'Package identity, version or architecture mismatch.'
Assert-Package ($manifest.Package.Properties.PublisherDisplayName -ceq $PublisherDisplayName) 'Package publisher display name differs from the selected identity.'
$families = @($manifest.SelectNodes('/f:Package/f:Dependencies/f:TargetDeviceFamily', $ns))
Assert-Package ($families.Count -eq 1 -and $families[0].Name -eq 'Windows.Desktop' -and $families[0].MinVersion -eq '10.0.22000.0' -and $families[0].MaxVersionTested -eq '10.0.26100.0') 'Windows 11 desktop minimum/tested OS mismatch.'
Assert-Package ($manifest.SelectNodes('/f:Package/f:Dependencies/f:PackageDependency', $ns).Count -eq 0) 'Unexpected external runtime framework dependency.'
$apps = @($manifest.SelectNodes('/f:Package/f:Applications/f:Application', $ns))
Assert-Package ($apps.Count -eq 1) 'Package must expose only Settings; Host is a companion executable, not a headless application.'
foreach ($app in $apps) {
    Assert-Package ($app.GetAttribute('RuntimeBehavior', $ns.LookupNamespace('uap10')) -eq 'packagedClassicApp' -and $app.GetAttribute('TrustLevel', $ns.LookupNamespace('uap10')) -eq 'mediumIL') 'Settings must be packaged classic mediumIL.'
    Assert-Package ($app.EntryPoint -eq 'Windows.FullTrustApplication') 'Unexpected application entry point.'
    Assert-Package (Test-Path -LiteralPath (Join-Path $unpacked $app.Executable)) 'Missing application executable.'
}
$settings = $manifest.SelectSingleNode("/f:Package/f:Applications/f:Application[@Id='Settings']", $ns)
Assert-Package ($null -ne $settings) 'Settings application ID changed.'
Assert-Package ($settings.Executable -eq 'WindowSweaters.Settings.exe' -and (Test-Path -LiteralPath (Join-Path $unpacked 'WindowSweaters.Host.exe'))) 'Companion executables must remain siblings at package root.'
Assert-Package ($settings.SelectSingleNode('uap:VisualElements', $ns).AppListEntry -eq 'default') 'Settings must be visible in Start; headless applications are not permitted.'
$tasks = @($manifest.SelectNodes('//desktop:StartupTask', $ns))
Assert-Package ($tasks.Count -eq 1 -and $tasks[0].TaskId -eq 'WindowSweatersStartup' -and $tasks[0].Enabled -eq 'false') 'Startup must be disabled by default.'
$startup = $settings.SelectSingleNode("f:Extensions/desktop:Extension[@Category='windows.startupTask']", $ns)
Assert-Package ($null -ne $startup -and $startup.Executable -eq 'WindowSweaters.Host.exe' -and $startup.EntryPoint -eq 'Windows.FullTrustApplication' -and $startup.SelectNodes('desktop:StartupTask', $ns).Count -eq 1) 'Settings must own the startup extension that launches Host.'
$capabilities = @($manifest.SelectNodes('/f:Package/f:Capabilities/*', $ns))
Assert-Package ($capabilities.Count -eq 1 -and $capabilities[0].Name -eq 'runFullTrust' -and $capabilities[0].NamespaceURI -eq 'http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities') 'Unexpected capability; only runFullTrust is permitted.'
Assert-Package (!(Test-Path (Join-Path $unpacked 'AppxSignature.p7x'))) 'Expected an unsigned artifact.'
$report = Get-Content (Join-Path $ArtifactDirectory 'payload-report.json') -Raw | ConvertFrom-Json
Assert-Package ($report.Platform -eq $Platform -and $report.NativeOutputRoot -eq $buildReceipt.NativeOutputRoot) 'Payload report does not match selected native build.'
foreach ($file in $report.Payload) {
    $path = Join-Path $unpacked $file.Path
    Assert-Package ((Test-Path -LiteralPath $path) -and (Get-FileHash -LiteralPath $path).Hash -eq $file.SHA256) "Package payload hash mismatch: $($file.Path)"
}
$allowed = @($report.Payload.Path) + @('AppxManifest.xml', 'SOURCE.txt', 'resources.pri', 'AppxBlockMap.xml', '[Content_Types].xml')
$files = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File)
foreach ($file in $files) {
    $relative = $file.FullName.Substring($unpacked.Length + 1)
    Assert-Package ($allowed -contains $relative -or $relative -eq 'AppxMetadata\CodeIntegrity.cat') "Unexpected packaged content: $relative"
}
Add-Type -AssemblyName System.Drawing
$assetSizes = [ordered]@{
    'StoreLogo.png' = @(50, 50); 'Square44x44Logo.png' = @(44, 44)
    'Square71x71Logo.png' = @(71, 71); 'Square150x150Logo.png' = @(150, 150)
    'Wide310x150Logo.png' = @(310, 150); 'Square310x310Logo.png' = @(310, 310)
}
foreach ($asset in $assetSizes.Keys) {
    $image = [Drawing.Image]::FromFile((Join-Path $unpacked "Assets\$asset"))
    try {
        Assert-Package ($image.Width -eq $assetSizes[$asset][0] -and $image.Height -eq $assetSizes[$asset][1]) "Wrong packaged asset dimensions: $asset"
    } finally { $image.Dispose() }
}
$machines = @()
foreach ($file in @($files | Where-Object Extension -in @('.exe', '.dll'))) {
    $relative = $file.FullName.Substring($unpacked.Length + 1)
    $machine = & (Join-Path $PSScriptRoot 'Test-PeArchitecture.ps1') -Path $file.FullName -Platform $Platform
    $machines += [ordered]@{ Path = $relative; Machine = $machine; SHA256 = (Get-FileHash -LiteralPath $file.FullName).Hash }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    foreach ($encoding in @([Text.Encoding]::ASCII, [Text.Encoding]::Unicode)) {
        Assert-Package ($encoding.GetString($bytes) -notmatch '(?i)[A-Z]:\\Users\\|/Users/|/home/') "Private home path in packaged binary: $relative"
    }
}
$resourceDump = Join-Path $PSScriptRoot "obj\$channel\$Version\$Platform\packaged-settings-resources.xml"
& (Join-Path $sdk 'makepri.exe') dump /if (Join-Path $unpacked 'resources.pri') /of $resourceDump /dt detailed /o | Out-Null
Assert-Package ($LASTEXITCODE -eq 0) 'SDK could not inspect the packaged Settings resource index.'
$resourceText = [IO.File]::ReadAllText($resourceDump)
foreach ($theme in @('Microsoft.UI.Xaml/Themes/themeresources.xbf', 'Microsoft.UI.Xaml/Themes/generic.xbf')) {
    Assert-Package ($resourceText.Contains($theme)) "Packaged application PRI lacks WinUI theme resource: $theme"
}
Remove-Item -LiteralPath $resourceDump -Force
$sourceZip = Join-Path $ArtifactDirectory $report.SourceArchive
Assert-Package ((Get-FileHash -LiteralPath $sourceZip).Hash -eq $report.SourceSHA256) 'Corresponding source archive hash mismatch.'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$source = [IO.Compression.ZipFile]::OpenRead($sourceZip)
try {
    $inventoryEntry = $source.GetEntry('packaging/SourceInventory.json')
    Assert-Package ($null -ne $inventoryEntry) 'Corresponding source inventory missing.'
    $reader = [IO.StreamReader]::new($inventoryEntry.Open())
    try { $inventory = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    Assert-Package (($inventory.Files | ConvertTo-Json -Depth 4 -Compress) -eq ($buildReceipt.SourceFiles | ConvertTo-Json -Depth 4 -Compress)) 'Corresponding source archive differs from the verified native build source.'
    foreach ($entry in $source.Entries) {
        Assert-Package ($entry.FullName -notmatch '(?i)(^|/)(\.packages|packages|build|obj|bin|out|artifacts|outputs|probe-results|TestResults|AppPackages|Generated Files|Debug|Release|x64|x86|ARM64|\.run-[^/]+)/|\.(user|suo|pdb|ilk|obj|pch|idb|res|exe|dll|lib|exp|winmd|tlog|binlog|log|pfx|p12|pem|key|cer|crt|der|p7b|p7x|nupkg|snupkg|msix|msixbundle|appx|appxbundle|appxupload|msixupload|appxsym|zip)$') "Private/generated file in source archive: $($entry.FullName)"
    }
    foreach ($required in @('LICENSE', 'NOTICE.md', 'WindowSweaters.sln', 'src/Host/WindowSweaters.Host.vcxproj', 'src/Settings/WindowSweaters.Settings.vcxproj', 'scripts/build.ps1', 'scripts/package.ps1', 'packaging/Package.appxmanifest', 'packaging/WindowSweaters.Package.wapproj')) {
        Assert-Package (@($source.Entries | Where-Object FullName -eq $required).Count -eq 1) "Missing corresponding source: $required"
    }
} finally { $source.Dispose() }
& (Join-Path $PSScriptRoot 'Test-SourceArchive.ps1') -SourceZip $sourceZip -InspectionDirectory (Join-Path $PSScriptRoot "obj\$channel\$Version\$Platform\source-roundtrip")
foreach ($exe in @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe', 'resources.pri')) {
    $current = Join-Path (Join-Path (Split-Path $PSScriptRoot -Parent) $buildReceipt.NativeOutputRoot) "$Platform\Release\$exe"
    $expectedInput = $report.ApplicationInputSHA256.PSObject.Properties[$exe].Value
    Assert-Package ((Get-FileHash -LiteralPath $current).Hash -eq $expectedInput) "Native input changed before final package validation: $exe"
}
$result = [ordered]@{
    Package = $package.Name; SHA256 = (Get-FileHash -LiteralPath $package.FullName).Hash
    Identity = $Name; Publisher = $Publisher; PublisherDisplayName = $PublisherDisplayName; Version = $Version; Architecture = $Platform.ToLowerInvariant(); Platform = $Platform; MinOS = '10.0.22000.0'
    NativeOutputRoot = $buildReceipt.NativeOutputRoot
    SourceFiles = $inventory.Files
    NativeInputFiles = $buildReceipt.Outputs
    ApplicationInputSHA256 = $report.ApplicationInputSHA256
    Applications = @('Settings visible'); StartupExecutable = 'WindowSweaters.Host.exe'; StartupEnabled = $false
    Capabilities = @('runFullTrust'); ExternalPackageDependencies = @()
    Signed = $false; Installed = $false; Published = $false
    ContentFiles = $files.Count; SourceArchive = $report.SourceArchive; SourceSHA256 = $report.SourceSHA256
    Binaries = $machines
    AssetDimensionsVerified = $true; PrivateApplicationHomePaths = 0
    NoGitSourceRoundtripVerified = $true
    SettingsThemeResourcesVerified = $true
}
$finalReceipt = & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -Platform $Platform -NativeOutputRoot $NativeOutputRoot
$validationJson = $result | ConvertTo-Json -Depth 4
& (Join-Path $PSScriptRoot 'Test-PackageBuildBinding.ps1') -ValidationReport ($validationJson | ConvertFrom-Json) -BuildReceipt $finalReceipt -SourceArchive $sourceZip
$validationJson | Set-Content (Join-Path $ArtifactDirectory 'validation-report.json') -Encoding UTF8
Write-Host "Validated unsigned $($package.Name): $Platform, Windows 11 22000+, visible Settings, disabled Host startup, runFullTrust only."
