[CmdletBinding()]
param(
    [ValidateSet('Development', 'Store')][string]$Channel = 'Development',
    [string]$StoreIdentityProps,
    [string]$Version = '0.1.0.0',
    [switch]$Bundle
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'Visual Studio Installer vswhere.exe is required.' }
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Install Visual Studio MSBuild and Windows Application Packaging Project tools.' }
$arguments = @(
    (Join-Path $root 'packaging\WindowSweaters.Package.wapproj'),
    '/nologo', '/v:minimal', '/t:Build', '/p:Configuration=Release', '/p:Platform=x64',
    "/p:PackageChannel=$Channel", "/p:PackageVersion=$Version", '/p:AppxPackageSigningEnabled=false'
)
if ($StoreIdentityProps) {
    $arguments += "/p:StoreIdentityProps=$((Resolve-Path -LiteralPath $StoreIdentityProps).Path)"
}
& $msbuild @arguments
if ($LASTEXITCODE -ne 0) { throw "Unsigned package build/validation failed (MSBuild exit $LASTEXITCODE)." }
if ($Bundle) {
    & (Join-Path $root 'packaging\New-Bundle.ps1') -ArtifactDirectory (Join-Path $root "packaging\artifacts\$Channel") -Version $Version
}
Write-Host "Unsigned $Channel artifacts: $(Join-Path $root "packaging\artifacts\$Channel")"
Write-Host 'Not signed, installed, clean-machine tested, uploaded, submitted or published.'
