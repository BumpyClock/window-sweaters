[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$id = [Guid]::NewGuid().ToString('N')
$root = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
$work = Join-Path $PSScriptRoot "obj\binding-contract-$id"
$nativeRoot = Join-Path $root "out\binding-contract-$id"
$artifactChannel = Join-Path $PSScriptRoot "artifacts\binding-contract-$id"
$artifactRoot = Join-Path $artifactChannel '0.1.0.0'
[void][IO.Directory]::CreateDirectory($work)
function Copy-Record([object]$Value) {
    $copy = $Value | ConvertTo-Json -Depth 7 | ConvertFrom-Json
    foreach ($record in $copy) { $record }
}
function Assert-Rejected([scriptblock]$Operation, [string]$Expected) {
    $rejected = $false
    try { & $Operation | Out-Null } catch {
        if ($_.Exception.Message -notlike $Expected) { throw }
        $rejected = $true
    }
    if (!$rejected) { throw "Expected rejection: $Expected" }
}
function Write-InventoryArchive([string]$Path, [object[]]$Files) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path }
    $zip = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $entry = $zip.CreateEntry('packaging/SourceInventory.json')
        $writer = [IO.StreamWriter]::new($entry.Open())
        try { $writer.Write(([ordered]@{ SchemaVersion = 1; Files = $Files } | ConvertTo-Json -Depth 5)) }
        finally { $writer.Dispose() }
    } finally { $zip.Dispose() }
}
function New-Validation([object]$Receipt, [string]$Source) {
    $inputs = [ordered]@{}
    foreach ($file in $Receipt.Outputs) {
        if ($file.Path -in @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe', 'resources.pri')) {
            $inputs[$file.Path] = $file.SHA256
        }
    }
    [pscustomobject][ordered]@{
        Platform = $Receipt.Platform; Architecture = $Receipt.Platform.ToLowerInvariant()
        NativeOutputRoot = $Receipt.NativeOutputRoot
        SourceFiles = @(Copy-Record $Receipt.SourceFiles); NativeInputFiles = @(Copy-Record $Receipt.Outputs)
        ApplicationInputSHA256 = ($inputs | ConvertTo-Json | ConvertFrom-Json)
        SourceArchive = [IO.Path]::GetFileName($Source); SourceSHA256 = (Get-FileHash -LiteralPath $Source).Hash
        Binaries = @([pscustomobject]@{ Path = 'WindowSweaters.Host.exe'; SHA256 = ('F' * 64) })
        Package = 'fixture.msix'; SHA256 = ('0' * 64)
        Identity = 'fixture'; Publisher = 'CN=fixture'; Version = '0.1.0.0'
        Signed = $false; NoGitSourceRoundtripVerified = $true; SettingsThemeResourcesVerified = $true
    }
}
try {
    $sampleSource = @([pscustomobject][ordered]@{ Path = 'src/provenance-fixture.cpp'; SHA256 = ('A' * 64) })
    $sampleInputs = @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe', 'resources.pri', 'runtime.dll') |
        ForEach-Object { [pscustomobject][ordered]@{ Path = $_; SHA256 = ('B' * 64) } }
    $source = Join-Path $work 'metadata-only-source.zip'
    Write-InventoryArchive $source $sampleSource
    foreach ($platform in @('x64', 'ARM64')) {
        $receipt = [pscustomobject]@{
            Platform = $platform; NativeOutputRoot = 'out\binding-fixture'
            SourceFiles = $sampleSource; Outputs = @(Copy-Record $sampleInputs)
        }
        $validation = New-Validation $receipt $source
        & (Join-Path $PSScriptRoot 'Test-PackageBuildBinding.ps1') -ValidationReport $validation -BuildReceipt $receipt -SourceArchive $source
        if ($validation.Binaries[0].SHA256 -eq $validation.ApplicationInputSHA256.'WindowSweaters.Host.exe') {
            throw 'Test must distinguish staged CodeView-normalized bytes from original native input.'
        }
        $newReceipt = Copy-Record $receipt
        $newReceipt.SourceFiles[0].SHA256 = 'C' * 64
        Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-PackageBuildBinding.ps1') -ValidationReport $validation -BuildReceipt $newReceipt -SourceArchive $source } 'Package source inventory differs*'
        $newReceipt = Copy-Record $receipt
        $newReceipt.Outputs[0].SHA256 = 'C' * 64
        Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-PackageBuildBinding.ps1') -ValidationReport $validation -BuildReceipt $newReceipt -SourceArchive $source } 'Package original native input differs*'
        $newReceipt = Copy-Record $receipt
        $newReceipt.Outputs[3].SHA256 = 'C' * 64
        Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-PackageBuildBinding.ps1') -ValidationReport $validation -BuildReceipt $newReceipt -SourceArchive $source } 'Package original native input inventory differs*'
    }
    $native = Join-Path $nativeRoot 'x64\Release'
    $artifacts = Join-Path $artifactRoot 'x64'
    [void][IO.Directory]::CreateDirectory($native)
    [void][IO.Directory]::CreateDirectory($artifacts)
    $outputs = @()
    foreach ($name in @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe', 'resources.pri')) {
        $path = Join-Path $native $name
        [IO.File]::WriteAllText($path, "original native input fixture: $name")
        $outputs += [pscustomobject][ordered]@{ Path = $name; SHA256 = (Get-FileHash -LiteralPath $path).Hash }
    }
    $inventory = & (Join-Path $PSScriptRoot 'Get-SourceInventory.ps1') -Root $root
    $receipt = [pscustomobject][ordered]@{
        SchemaVersion = 1; Platform = 'x64'; Configuration = 'Release'
        NativeOutputRoot = $nativeRoot.Substring($root.Length + 1)
        SourceFiles = $inventory.Files; Outputs = $outputs
    }
    $receiptPath = Join-Path $native 'native-build-report.json'
    $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    $withoutSlash = & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -NativeOutputRoot $nativeRoot
    $withSlash = & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -NativeOutputRoot ($nativeRoot + '\')
    if (($withoutSlash | ConvertTo-Json -Depth 6 -Compress) -cne ($withSlash | ConvertTo-Json -Depth 6 -Compress)) {
        throw 'Native output root with/without trailing separator must bind to the same canonical receipt.'
    }
    $source = Join-Path $artifacts 'metadata-only-source.zip'
    Write-InventoryArchive $source $inventory.Files
    $validation = New-Validation $receipt $source
    $package = Join-Path $artifacts $validation.Package
    [IO.File]::WriteAllText($package, 'unchanged old package contract fixture')
    $validation.SHA256 = (Get-FileHash -LiteralPath $package).Hash
    $validationPath = Join-Path $artifacts 'validation-report.json'
    $validation | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $validationPath -Encoding UTF8
    [IO.File]::WriteAllText((Join-Path $native 'WindowSweaters.Host.exe'), 'new build of the same package version')
    $receipt.Outputs[0].SHA256 = (Get-FileHash -LiteralPath (Join-Path $native 'WindowSweaters.Host.exe')).Hash
    $receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    $oldPackageHash = (Get-FileHash -LiteralPath $package).Hash
    $oldSourceHash = (Get-FileHash -LiteralPath $source).Hash
    Assert-Rejected { & (Join-Path $PSScriptRoot 'New-Bundle.ps1') -ArtifactDirectory $artifactRoot -Version '0.1.0.0' -Architectures x64 } 'Package original native input differs*'
    if ((Get-FileHash -LiteralPath $package).Hash -ne $oldPackageHash -or (Get-FileHash -LiteralPath $source).Hash -ne $oldSourceHash) {
        throw 'Stale provenance rejection must preserve old package and corresponding source bytes.'
    }
    $oldSourceFiles = @(Copy-Record $receipt.SourceFiles)
    $oldSourceFiles[0].SHA256 = '0' * 64
    Write-InventoryArchive $source $oldSourceFiles
    $validation.SourceFiles = $oldSourceFiles
    $validation.SourceSHA256 = (Get-FileHash -LiteralPath $source).Hash
    $validation | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $validationPath -Encoding UTF8
    $oldSourceHash = $validation.SourceSHA256
    Assert-Rejected { & (Join-Path $PSScriptRoot 'New-Bundle.ps1') -ArtifactDirectory $artifactRoot -Version '0.1.0.0' -Architectures x64 } 'Package source inventory differs*'
    if ((Get-FileHash -LiteralPath $package).Hash -ne $oldPackageHash -or (Get-FileHash -LiteralPath $source).Hash -ne $oldSourceHash) {
        throw 'Old source/new receipt rejection must preserve archived input bytes.'
    }
} finally {
    foreach ($owned in @($work, $nativeRoot, $artifactChannel)) {
        if (Test-Path -LiteralPath $owned) { Remove-Item -LiteralPath $owned -Recurse -Force }
    }
}
Write-Host 'PASS: both architecture provenance bindings; original versus staged EXE hashes; stale source/application/runtime receipt rejection; actual New-Bundle rejects unchanged old MSIX/source with a new receipt; trailing-root equivalence.'
