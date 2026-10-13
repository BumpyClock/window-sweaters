[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Platforms,
    [ValidateSet('Development', 'Store')][string]$Channel = 'Development',
    [string]$Version = '0.1.0.0',
    [string]$StoreIdentityProps,
    [string]$NativeOutputRoot,
    [Parameter(Mandatory)][string]$MSBuild,
    [Parameter(Mandatory)][string]$PackageInfoPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Platforms -notmatch '^(?i)(x64|arm64)(\|(x64|arm64))*$') {
    throw 'AppxBundlePlatforms must contain x64, ARM64 or both, separated by |.'
}
$architectures = @($Platforms.Split('|') | ForEach-Object {
    if ($_ -ieq 'arm64') { 'ARM64' } else { 'x64' }
})
if (@($architectures | Sort-Object -Unique).Count -ne $architectures.Count) {
    throw 'AppxBundlePlatforms must not contain duplicate architectures.'
}
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$' -or @($Version.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) {
    throw 'PackageVersion must contain four integers between 0 and 65535.'
}
if ($Channel -eq 'Development') { $StoreIdentityProps = $null }
$root = Split-Path $PSScriptRoot -Parent
$buildArguments = @{}
if ($NativeOutputRoot -and
    [IO.Path]::GetFullPath($NativeOutputRoot).TrimEnd('\') -ne [IO.Path]::GetFullPath((Join-Path $root 'out')).TrimEnd('\')) {
    $buildArguments.NativeOutputRoot = $NativeOutputRoot
}
foreach ($platform in $architectures) {
    & (Join-Path $root 'scripts\build.ps1') -Configuration Release -Platform $platform -MSBuild $MSBuild @buildArguments
}
& (Join-Path $root 'scripts\package.ps1') -Channel $Channel -Version $Version -Architectures $architectures `
    -StoreIdentityProps $StoreIdentityProps -NativeOutputRoot $NativeOutputRoot -MSBuild $MSBuild -Bundle
$directory = Join-Path $PSScriptRoot "artifacts\$Channel\$Version"
$report = Get-Content -LiteralPath (Join-Path $directory 'bundle-validation-report.json') -Raw | ConvertFrom-Json
$label = ($architectures | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) -join '_'
$name = "WindowSweaters.Package_${Version}_$label.msixbundle"
$bundle = Join-Path $directory $name
if ($report.Bundle -ne $name -or $report.Version -ne $Version -or $report.Signed -ne $false -or
    ($report.Architectures -join '|') -ne (($architectures | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object) -join '|') -or
    (Get-FileHash -LiteralPath $bundle).Hash -ne $report.SHA256) {
    throw 'Visual Studio bundle output differs from the validated architecture/version/hash.'
}
[void][IO.Directory]::CreateDirectory((Split-Path ([IO.Path]::GetFullPath($PackageInfoPath)) -Parent))
[IO.File]::WriteAllText($PackageInfoPath, $bundle + [Environment]::NewLine)
