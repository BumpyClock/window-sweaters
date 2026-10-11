[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArtifactDirectory,
    [Parameter(Mandatory)][string]$Version,
    [ValidateSet('x64', 'ARM64')][string[]]$Architectures = @('x64', 'ARM64')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$' -or @($Version.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) {
    throw 'PackageVersion must contain four integers between 0 and 65535.'
}
$artifactRoot = [IO.Path]::GetFullPath($ArtifactDirectory).TrimEnd('\')
if (!$artifactRoot.StartsWith(([IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'artifacts')) + '\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Bundle artifacts must remain under packaging\artifacts.'
}
$channel = [IO.Path]::GetFileName((Split-Path $artifactRoot -Parent))
$expectedArtifact = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "artifacts\$channel\$Version")).TrimEnd('\')
if ($artifactRoot -ne $expectedArtifact) { throw 'Bundle artifacts must use the selected channel/version directory.' }
if (!$Architectures.Count -or @($Architectures | Sort-Object -Unique).Count -ne $Architectures.Count) {
    throw 'Bundle architectures must be unique and nonempty.'
}
$validated = @{}
$packages = @(Get-ChildItem -LiteralPath $artifactRoot -Recurse -Filter '*.msix')
if ($packages.Count -ne $Architectures.Count) { throw 'Bundle inputs must contain exactly one application MSIX per requested architecture, and no extra packages.' }
foreach ($platform in $Architectures) {
    $directory = Join-Path $artifactRoot $platform
    $validation = Get-Content -LiteralPath (Join-Path $directory 'validation-report.json') -Raw | ConvertFrom-Json
    $nativeRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) $validation.NativeOutputRoot)).TrimEnd('\')
    $receipt = & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -Platform $platform -NativeOutputRoot $nativeRoot
    $matches = @($packages | Where-Object {
        $_.Name -eq $validation.Package -and $_.FullName.StartsWith(($directory + '\'), [StringComparison]::OrdinalIgnoreCase)
    })
    if ($matches.Count -ne 1 -or $validation.Version -ne $Version -or $validation.Platform -ne $platform -or
        $validation.Architecture -ne $platform.ToLowerInvariant() -or $validation.Signed -ne $false -or
        !$validation.NoGitSourceRoundtripVerified -or !$validation.SettingsThemeResourcesVerified -or
        (Get-FileHash -LiteralPath $matches[0].FullName).Hash -ne $validation.SHA256) {
        throw "Bundle requires one currently validated unsigned $platform MSIX of the requested version."
    }
    $source = Join-Path $directory $validation.SourceArchive
    if ((Get-FileHash -LiteralPath $source).Hash -ne $validation.SourceSHA256) {
        throw "Corresponding source changed after $platform MSIX validation."
    }
    & (Join-Path $PSScriptRoot 'Test-PackageBuildBinding.ps1') -ValidationReport $validation -BuildReceipt $receipt -SourceArchive $source
    $validated[$platform.ToLowerInvariant()] = @{ Report = $validation; Package = $matches[0] }
}
$first = $validated[$Architectures[0].ToLowerInvariant()].Report
foreach ($entry in $validated.Values) {
    if ($entry.Report.Identity -ne $first.Identity -or $entry.Report.Publisher -ne $first.Publisher -or
        $entry.Report.SourceSHA256 -ne $first.SourceSHA256) {
        throw 'All bundle inputs must have identical identity, publisher, version and corresponding-source hashes.'
    }
}
$label = ($Architectures | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) -join '_'
$inputDirectory = Join-Path $PSScriptRoot "obj\$channel\$Version\bundle-$label\input"
$inspection = Join-Path $PSScriptRoot "obj\$channel\$Version\bundle-$label\inspection"
foreach ($directory in @($inputDirectory, $inspection)) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
}
[void][IO.Directory]::CreateDirectory($inputDirectory)
foreach ($entry in $validated.Values) {
    Copy-Item -LiteralPath $entry.Package.FullName -Destination $inputDirectory
}
$bundle = Join-Path $artifactRoot "WindowSweaters.Package_${Version}_$label.msixbundle"
if (Test-Path -LiteralPath $bundle) {
    $existing = [IO.Compression.ZipFile]::OpenRead($bundle)
    try {
        if ($existing.GetEntry('AppxSignature.p7x')) { throw 'Refusing to overwrite a signed bundle.' }
    } finally { $existing.Dispose() }
}
$makeappx = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\makeappx.exe'
if (!(Test-Path -LiteralPath $makeappx)) { throw "Missing packaging tool: $makeappx" }
& $makeappx bundle /d $inputDirectory /p $bundle /bv $Version /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'MakeAppx bundle validation failed.' }
& $makeappx unbundle /p $bundle /d $inspection /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'MakeAppx unbundle validation failed.' }
$inner = @(Get-ChildItem -LiteralPath $inspection -Recurse -File | Where-Object Extension -in @('.msix', '.appx'))
if ($inner.Count -ne $Architectures.Count) { throw 'Unbundled package count differs from requested architectures.' }
$archive = [IO.Compression.ZipFile]::OpenRead($bundle)
$innerReports = @()
try {
    if ($archive.GetEntry('AppxSignature.p7x')) { throw 'Expected an unsigned bundle.' }
    $manifestEntry = $archive.GetEntry('AppxMetadata/AppxBundleManifest.xml')
    if (!$manifestEntry) { throw 'Bundle manifest is missing.' }
    $reader = [IO.StreamReader]::new($manifestEntry.Open())
    try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $identity = $manifest.Bundle.Identity
    if ($identity.Name -ne $first.Identity -or $identity.Publisher -ne $first.Publisher -or $identity.Version -ne $Version) {
        throw 'Bundle identity/version differs from the validated application packages.'
    }
    $contained = @($manifest.Bundle.Packages.Package)
    $embeddedPackages = @($archive.Entries | Where-Object FullName -match '(?i)\.(msix|appx)$')
    if ($contained.Count -ne $Architectures.Count -or $embeddedPackages.Count -ne $Architectures.Count) {
        throw 'Bundle must contain exactly the requested application packages.'
    }
    $seen = @{}
    foreach ($package in $contained) {
        $architecture = [string]$package.Architecture
        if (!$validated.ContainsKey($architecture) -or $seen.ContainsKey($architecture) -or $package.Type -ne 'application') {
            throw 'Bundle contains a duplicate, unexpected architecture or non-application package.'
        }
        $seen[$architecture] = $true
        $report = $validated[$architecture].Report
        $matches = @($inner | Where-Object Name -eq $package.FileName)
        if ($package.FileName -ne $report.Package -or !$archive.GetEntry($package.FileName) -or $matches.Count -ne 1 -or
            (Get-FileHash -LiteralPath $matches[0].FullName).Hash -ne $report.SHA256) {
            throw "Unbundled $architecture MSIX differs from its independently validated input."
        }
        $msix = [IO.Compression.ZipFile]::OpenRead($matches[0].FullName)
        try {
            if ($msix.GetEntry('AppxSignature.p7x')) { throw "Expected unsigned $architecture application package." }
            $reader = [IO.StreamReader]::new($msix.GetEntry('AppxManifest.xml').Open())
            try { [xml]$appManifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
            $appIdentity = $appManifest.Package.Identity
            if ($appIdentity.ProcessorArchitecture -ne $architecture -or $appIdentity.Name -ne $first.Identity -or
                $appIdentity.Publisher -ne $first.Publisher -or $appIdentity.Version -ne $Version) {
                throw "Unbundled $architecture identity differs from its validation report."
            }
        } finally { $msix.Dispose() }
        $innerReports += [ordered]@{
            Platform = $report.Platform; Architecture = $architecture; Package = $report.Package; SHA256 = $report.SHA256; Signed = $false
            ApplicationInputSHA256 = $report.ApplicationInputSHA256; NativeInputFiles = $report.NativeInputFiles
        }
    }
} finally { $archive.Dispose() }
foreach ($entry in $validated.Values) {
    if ((Get-FileHash -LiteralPath $entry.Package.FullName).Hash -ne $entry.Report.SHA256) { throw 'MSIX input changed during bundling.' }
    $nativeRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) $entry.Report.NativeOutputRoot)).TrimEnd('\')
    $receipt = & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -Platform $entry.Report.Platform -NativeOutputRoot $nativeRoot
    $source = Join-Path (Join-Path $artifactRoot $entry.Report.Platform) $entry.Report.SourceArchive
    & (Join-Path $PSScriptRoot 'Test-PackageBuildBinding.ps1') -ValidationReport $entry.Report -BuildReceipt $receipt -SourceArchive $source
}
[ordered]@{
    Bundle = [IO.Path]::GetFileName($bundle); SHA256 = (Get-FileHash -LiteralPath $bundle).Hash
    Identity = $first.Identity; Publisher = $first.Publisher; Version = $Version
    Architectures = @($innerReports.Architecture | Sort-Object)
    InnerPackages = @($innerReports | Sort-Object Architecture); SourceSHA256 = $first.SourceSHA256
    SourceFiles = $first.SourceFiles
    Signed = $false; Installed = $false; Published = $false; ARM64RuntimeTested = $false
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $artifactRoot 'bundle-validation-report.json') -Encoding UTF8
Write-Host "Validated unsigned bundle: $([IO.Path]::GetFileName($bundle)); $($Architectures -join ', '); inner MSIX bytes preserved."
