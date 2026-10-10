[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArtifactDirectory,
    [Parameter(Mandatory)][string]$Version
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$artifactRoot = [IO.Path]::GetFullPath($ArtifactDirectory)
if (!$artifactRoot.StartsWith(([IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'artifacts')) + '\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Bundle artifacts must remain under packaging\artifacts.'
}
$validation = Get-Content (Join-Path $artifactRoot 'validation-report.json') -Raw | ConvertFrom-Json
$packages = @(Get-ChildItem -LiteralPath $artifactRoot -Recurse -Filter $validation.Package)
if ($packages.Count -ne 1 -or $validation.Version -ne $Version -or (Get-FileHash -LiteralPath $packages[0].FullName).Hash -ne $validation.SHA256) {
    throw 'Bundle requires exactly one currently validated x64 MSIX of the requested version.'
}
$inputDirectory = Join-Path $PSScriptRoot 'obj\bundle-input'
$inspection = Join-Path $PSScriptRoot 'obj\bundle-inspection'
foreach ($directory in @($inputDirectory, $inspection)) {
    if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
}
[void][IO.Directory]::CreateDirectory($inputDirectory)
Copy-Item -LiteralPath $packages[0].FullName -Destination $inputDirectory
$bundle = Join-Path $artifactRoot "WindowSweaters.Package_${Version}_x64.msixbundle"
$makeappx = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\makeappx.exe'
& $makeappx bundle /d $inputDirectory /p $bundle /bv $Version /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'MakeAppx bundle validation failed.' }
& $makeappx unbundle /p $bundle /d $inspection /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'MakeAppx unbundle validation failed.' }
$inner = @(Get-ChildItem -LiteralPath $inspection -Filter '*.msix')
if ($inner.Count -ne 1 -or (Get-FileHash -LiteralPath $inner[0].FullName).Hash -ne $validation.SHA256) {
    throw 'Bundle content is not the verified unsigned x64 MSIX.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($bundle)
try {
    if (@($archive.Entries | Where-Object FullName -eq 'AppxSignature.p7x').Count) { throw 'Expected an unsigned bundle.' }
    $manifestEntry = $archive.GetEntry('AppxMetadata/AppxBundleManifest.xml')
    if (!$manifestEntry) { throw 'Bundle manifest is missing.' }
    $reader = New-Object IO.StreamReader($manifestEntry.Open())
    try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $bundleIdentity = $manifest.Bundle.Identity
    if ($bundleIdentity.Name -ne $validation.Identity -or $bundleIdentity.Publisher -ne $validation.Publisher -or $bundleIdentity.Version -ne $Version) {
        throw 'Bundle identity/version differs from the validated application package.'
    }
    $contained = @($manifest.Bundle.Packages.Package)
    if ($contained.Count -ne 1 -or $contained[0].Architecture -ne 'x64' -or $contained[0].Type -ne 'application') {
        throw 'Bundle must contain exactly one x64 application package.'
    }
} finally { $archive.Dispose() }
$result = [ordered]@{
    Bundle = [IO.Path]::GetFileName($bundle); SHA256 = (Get-FileHash -LiteralPath $bundle).Hash
    Version = $Version; InnerPackage = $validation.Package; InnerSHA256 = $validation.SHA256
    Signed = $false; Installed = $false; Published = $false
}
$result | ConvertTo-Json | Set-Content (Join-Path $artifactRoot 'bundle-validation-report.json') -Encoding UTF8
Write-Host "Validated optional unsigned bundle: $([IO.Path]::GetFileName($bundle))"
