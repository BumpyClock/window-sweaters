[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Destination
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$inventory = & (Join-Path $PSScriptRoot 'Get-SourceInventory.ps1') -Root $Root
$destinationPath = [IO.Path]::GetFullPath($Destination)
$owned = @((Join-Path $PSScriptRoot 'obj'), (Join-Path $PSScriptRoot 'artifacts'))
if (!@($owned | Where-Object { $destinationPath.StartsWith(([IO.Path]::GetFullPath($_) + '\'), [StringComparison]::OrdinalIgnoreCase) }).Count) {
    throw 'Source archives must remain under packaging\obj or packaging\artifacts.'
}
[void][IO.Directory]::CreateDirectory((Split-Path $destinationPath -Parent))
if (Test-Path -LiteralPath $destinationPath) { Remove-Item -LiteralPath $destinationPath -Force }
$zip = [IO.Compression.ZipFile]::Open($destinationPath, [IO.Compression.ZipArchiveMode]::Create)
$sha = [Security.Cryptography.SHA256]::Create()
try {
    foreach ($file in $inventory.Files) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $Root $file.Path.Replace('/', '\')))
        $file.SHA256 = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '')
        $entry = $zip.CreateEntry($file.Path, [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [DateTimeOffset]::new(2020, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        $stream = $entry.Open()
        try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
    }
    $entry = $zip.CreateEntry('packaging/SourceInventory.json', [IO.Compression.CompressionLevel]::Optimal)
    $entry.LastWriteTime = [DateTimeOffset]::new(2020, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
    $writer = New-Object IO.StreamWriter($entry.Open(), (New-Object Text.UTF8Encoding($false)))
    try { $writer.Write(($inventory | ConvertTo-Json -Depth 4)) } finally { $writer.Dispose() }
} finally { $sha.Dispose(); $zip.Dispose() }
$inventory
