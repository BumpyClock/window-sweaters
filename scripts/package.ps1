[CmdletBinding()]
param(
    [ValidateSet('Development', 'Store')][string]$Channel = 'Development',
    [string]$StoreIdentityProps,
    [string]$Version = '0.1.0.0',
    [ValidateSet('x64', 'ARM64')][string[]]$Architectures = @('x64', 'ARM64'),
    [string]$NativeOutputRoot,
    [switch]$Bundle
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$' -or @($Version.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) {
    throw 'PackageVersion must contain four integers between 0 and 65535.'
}
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
if ($NativeOutputRoot) { $NativeOutputRoot = [IO.Path]::GetFullPath($NativeOutputRoot).TrimEnd('\') }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'Visual Studio Installer vswhere.exe is required.' }
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Install Visual Studio MSBuild and Windows Application Packaging Project tools.' }
$vs = [IO.Path]::GetFullPath((Join-Path (Split-Path $msbuild -Parent) '..\..\..'))
foreach ($required in @(
    (Join-Path $vs 'MSBuild\Microsoft\DesktopBridge\Microsoft.DesktopBridge.targets'),
    (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\makeappx.exe')
)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing Windows Application Packaging Project tools or SDK 10.0.26100.0: $required" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (!$Architectures.Count -or @($Architectures | Sort-Object -Unique).Count -ne $Architectures.Count) {
    throw 'Architectures must contain one or both unique supported platforms: x64, ARM64.'
}
foreach ($platform in $Architectures) {
    $artifactDirectory = Join-Path $root "packaging\artifacts\$Channel\$Version\$platform"
    if (Test-Path -LiteralPath $artifactDirectory) {
        foreach ($existing in Get-ChildItem -LiteralPath $artifactDirectory -Recurse -Filter '*.msix') {
            $archive = [IO.Compression.ZipFile]::OpenRead($existing.FullName)
            try {
                if ($archive.GetEntry('AppxSignature.p7x')) { throw "Refusing to rebuild over a signed package: $($existing.Name)" }
            } finally { $archive.Dispose() }
        }
    }
    [void](& (Join-Path $root 'packaging\Test-NativeBuild.ps1') -Platform $platform -NativeOutputRoot $NativeOutputRoot)
}
foreach ($platform in $Architectures) {
    $arguments = @(
        (Join-Path $root 'packaging\WindowSweaters.Package.wapproj'),
        '/nologo', '/v:minimal', '/t:Build', '/p:Configuration=Release', "/p:Platform=$platform",
        "/p:PackageChannel=$Channel", "/p:PackageVersion=$Version", '/p:AppxPackageSigningEnabled=false'
    )
    if ($NativeOutputRoot) { $arguments += "/p:NativeOutputRoot=$NativeOutputRoot" }
    if ($StoreIdentityProps) {
        $arguments += "/p:StoreIdentityProps=$((Resolve-Path -LiteralPath $StoreIdentityProps).Path)"
    }
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) { throw "Unsigned $platform package build/validation failed (MSBuild exit $LASTEXITCODE)." }
}
if ($Bundle) {
    & (Join-Path $root 'packaging\New-Bundle.ps1') -ArtifactDirectory (Join-Path $root "packaging\artifacts\$Channel\$Version") -Version $Version -Architectures $Architectures
}
Write-Host "Unsigned $Channel artifacts: $(Join-Path $root "packaging\artifacts\$Channel\$Version")"
Write-Host 'Not signed, installed, ARM64 runtime-tested, clean-machine tested, uploaded, submitted or published.'
