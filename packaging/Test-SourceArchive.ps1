[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceZip)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
$extracted = Join-Path $PSScriptRoot 'obj\source-roundtrip'
if (Test-Path -LiteralPath $extracted) { Remove-Item -LiteralPath $extracted -Recurse -Force }
[void][IO.Directory]::CreateDirectory($extracted)
$zip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($SourceZip))
try {
    $inventoryEntry = $zip.GetEntry('packaging/SourceInventory.json')
    if (!$inventoryEntry) { throw 'Corresponding source ZIP lacks its inventory.' }
    $reader = New-Object IO.StreamReader($inventoryEntry.Open())
    try { $inventoryText = $reader.ReadToEnd() } finally { $reader.Dispose() }
    $expected = $inventoryText | ConvertFrom-Json
    $declared = @($expected.Files.Path) + 'packaging/SourceInventory.json'
    if ($zip.Entries.Count -ne $declared.Count) { throw 'ZIP entry count differs from the declared inventory.' }
    $seen = @{}
    foreach ($entry in $zip.Entries) {
        $relative = $entry.FullName
        if ($relative -notin $declared -or $seen.ContainsKey($relative) -or
            $relative -match '(^|/)\.\.?(/|$)|[\\:<>|?*\x00-\x1f]' -or $relative.StartsWith('/')) {
            throw "Unexpected/unsafe source archive entry: $relative"
        }
        $seen[$relative] = $true
        $destination = Join-Path $extracted $relative.Replace('/', '\')
        [void][IO.Directory]::CreateDirectory((Split-Path $destination -Parent))
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination)
    }
} finally { $zip.Dispose() }
if (Test-Path (Join-Path $extracted '.git')) { throw 'Source archive unexpectedly contains Git metadata.' }
$enumerator = Join-Path $extracted 'packaging\Get-SourceInventory.ps1'
$savedPath = $env:PATH
try {
    $env:PATH = ''
    $actual = & $enumerator -Root $extracted -ValidateHashes
    $rebuiltZip = Join-Path $extracted 'packaging\obj\rebuilt-source.zip'
    [void](& (Join-Path $extracted 'packaging\New-SourceArchive.ps1') -Root $extracted -Destination $rebuiltZip)
    if ((Get-FileHash -LiteralPath $rebuiltZip).Hash -ne (Get-FileHash -LiteralPath $SourceZip).Hash) {
        throw 'No-Git recipient cannot reproduce the corresponding source ZIP byte-for-byte.'
    }
} finally { $env:PATH = $savedPath }
if (($actual | ConvertTo-Json -Depth 4 -Compress) -ne ($expected | ConvertTo-Json -Depth 4 -Compress)) {
    throw 'No-Git source enumeration differs from the distributed inventory.'
}
$inventoryPath = Join-Path $extracted 'packaging\SourceInventory.json'
foreach ($invalidPath in @('../escape.txt', 'packaging/secret.props.user', $expected.Files[1].Path)) {
    $invalidInventory = $inventoryText | ConvertFrom-Json
    $invalidInventory.Files[0].Path = $invalidPath
    [IO.File]::WriteAllText($inventoryPath, ($invalidInventory | ConvertTo-Json -Depth 4))
    $rejected = $false
    try { [void](& $enumerator -Root $extracted) } catch {
        if ($_.Exception.Message -notlike 'Invalid/duplicate source inventory entry*') { throw }
        $rejected = $true
    } finally { [IO.File]::WriteAllText($inventoryPath, $inventoryText) }
    if (!$rejected) { throw "No-Git inventory accepted an unsafe/duplicate entry: $invalidPath" }
}
$unexpected = Join-Path $extracted 'src\UnexpectedSource.txt'
[IO.File]::WriteAllText($unexpected, 'not declared in the corresponding source archive')
$rejected = $false
try { [void](& $enumerator -Root $extracted) } catch {
    if ($_.Exception.Message -notlike 'Unexpected file not declared*') { throw }
    $rejected = $true
} finally { Remove-Item -LiteralPath $unexpected -Force }
if (!$rejected) { throw 'No-Git inventory accepted an unexpected source file.' }
$generated = Join-Path $extracted 'out\x64\Release\ignored.log'
[void][IO.Directory]::CreateDirectory((Split-Path $generated -Parent))
[IO.File]::WriteAllText($generated, 'restored build output must not enter the source inventory')
$withOutput = & $enumerator -Root $extracted -ValidateHashes
if (($withOutput | ConvertTo-Json -Depth 4 -Compress) -ne ($expected | ConvertTo-Json -Depth 4 -Compress)) {
    throw 'No-Git source enumeration included restored build outputs.'
}
Remove-Item (Join-Path $extracted 'out') -Recurse -Force
Remove-Item (Join-Path $extracted 'packaging\obj') -Recurse -Force
Write-Host "Verified no-Git source roundtrip: $($actual.Files.Count) files; ZIP reproduced byte-for-byte with empty PATH; unexpected source rejected; generated output excluded."
