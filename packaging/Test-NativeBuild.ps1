[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [string]$NativeOutputRoot
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
$owned = [IO.Path]::GetFullPath((Join-Path $root 'out')).TrimEnd('\')
$outputRoot = if ($NativeOutputRoot) { [IO.Path]::GetFullPath($NativeOutputRoot).TrimEnd('\') } else { $owned }
if ($outputRoot -ne $owned -and !$outputRoot.StartsWith(($owned + '\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'NativeOutputRoot must remain inside repository out.'
}
$output = Join-Path $outputRoot "$Platform\Release"
$receiptPath = Join-Path $output 'native-build-report.json'
if (!(Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
    throw "Missing native build receipt. Run scripts\build.ps1 -Configuration Release -Platform $Platform with the selected NativeOutputRoot."
}
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
if ($receipt.SchemaVersion -ne 1 -or $receipt.Platform -ne $Platform -or $receipt.Configuration -ne 'Release' -or
    $receipt.NativeOutputRoot -ne $outputRoot.Substring($root.Length + 1)) {
    throw 'Native build receipt configuration/output path differs from the requested package input.'
}
$source = & (Join-Path $PSScriptRoot 'Get-SourceInventory.ps1') -Root $root -ValidateHashes
if (($source.Files | ConvertTo-Json -Depth 4 -Compress) -ne ($receipt.SourceFiles | ConvertTo-Json -Depth 4 -Compress)) {
    throw 'Native build source hashes differ from current corresponding source. Freeze all source writers, then rebuild both architectures.'
}
$seen = @{}
foreach ($file in $receipt.Outputs) {
    if (!$file.Path -or [IO.Path]::IsPathRooted($file.Path) -or $file.Path -match '(^|\\)\.\.?($|\\)|[/:]' -or
        $file.SHA256 -notmatch '^[A-Fa-f0-9]{64}$' -or $seen.ContainsKey($file.Path)) {
        throw 'Invalid native build output receipt.'
    }
    $seen[$file.Path] = $true
    $path = Join-Path $output $file.Path
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path).Hash -ne $file.SHA256) {
        throw "Native build output changed: $($file.Path). Rebuild before packaging."
    }
}
foreach ($required in @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe', 'resources.pri')) {
    if (!$seen.ContainsKey($required)) { throw "Missing native build output receipt: $required" }
}
$receipt
