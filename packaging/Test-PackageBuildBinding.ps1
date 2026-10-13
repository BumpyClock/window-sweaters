[CmdletBinding()]
param(
    [Parameter(Mandatory)][object]$ValidationReport,
    [Parameter(Mandatory)][object]$BuildReceipt,
    [Parameter(Mandatory)][string]$SourceArchive
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
foreach ($field in @('SourceFiles', 'NativeInputFiles', 'ApplicationInputSHA256', 'Platform', 'NativeOutputRoot', 'SourceSHA256')) {
    if (!$ValidationReport.PSObject.Properties[$field]) { throw "Package validation report lacks native build provenance: $field" }
}
if ($ValidationReport.Platform -ne $BuildReceipt.Platform -or $ValidationReport.NativeOutputRoot -ne $BuildReceipt.NativeOutputRoot) {
    throw 'Package native build platform/output provenance differs from current receipt.'
}
$currentSource = $BuildReceipt.SourceFiles | ConvertTo-Json -Depth 4 -Compress
$packageSource = $ValidationReport.SourceFiles | ConvertTo-Json -Depth 4 -Compress
if ($packageSource -cne $currentSource) { throw 'Package source inventory differs from current native build receipt.' }
if ((Get-FileHash -LiteralPath $SourceArchive).Hash -ne $ValidationReport.SourceSHA256) {
    throw 'Corresponding source archive differs from the package validation report.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($SourceArchive))
try {
    $entry = $archive.GetEntry('packaging/SourceInventory.json')
    if (!$entry) { throw 'Corresponding source archive lacks its inventory.' }
    $reader = [IO.StreamReader]::new($entry.Open())
    try { $inventory = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($inventory.SchemaVersion -ne 1 -or ($inventory.Files | ConvertTo-Json -Depth 4 -Compress) -cne $currentSource) {
        throw 'Archived source inventory differs from current native build receipt.'
    }
} finally { $archive.Dispose() }
$outputs = @{}
foreach ($file in $BuildReceipt.Outputs) { $outputs[$file.Path] = $file.SHA256 }
$required = @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe', 'resources.pri')
$inputs = @($ValidationReport.ApplicationInputSHA256.PSObject.Properties | ForEach-Object { $_.Name })
if ($inputs.Count -ne $required.Count -or @($inputs | Where-Object { $_ -notin $required }).Count) {
    throw "Package validation report must record exactly the original Host, Settings and PRI input hashes; received: $($inputs -join ', ')."
}
foreach ($path in $required) {
    $inputHash = $ValidationReport.ApplicationInputSHA256.PSObject.Properties[$path].Value
    if (!$outputs.ContainsKey($path) -or $inputHash -notmatch '^[A-Fa-f0-9]{64}$' -or $inputHash -ne $outputs[$path]) {
        throw "Package original native input differs from current build receipt: $path"
    }
}
$packageInputs = $ValidationReport.NativeInputFiles | Sort-Object Path | ConvertTo-Json -Depth 4 -Compress
$buildInputs = $BuildReceipt.Outputs | Sort-Object Path | ConvertTo-Json -Depth 4 -Compress
if ($packageInputs -cne $buildInputs) { throw 'Package original native input inventory differs from current build receipt.' }
