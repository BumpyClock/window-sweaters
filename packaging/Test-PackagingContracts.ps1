[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$directory = Join-Path $PSScriptRoot 'obj\contract-tests'
$nativeRoot = $null
$sourceFixture = Join-Path $directory ('source-inventory-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($directory)
function Assert-Rejected([scriptblock]$Operation, [string]$Expected) {
    $rejected = $false
    try { & $Operation | Out-Null } catch {
        if ($_.Exception.Message -notlike $Expected) { throw }
        $rejected = $true
    }
    if (!$rejected) { throw "Expected rejection: $Expected" }
}
try {
    $root = Split-Path $PSScriptRoot -Parent
    foreach ($relative in @('LICENSE', 'NOTICE.md', 'WindowSweaters.sln', 'src\Host\WindowSweaters.Host.vcxproj',
        'src\Settings\WindowSweaters.Settings.vcxproj', 'scripts\build.ps1', 'scripts\package.ps1',
        'packaging\Package.appxmanifest', 'packaging\WindowSweaters.Package.wapproj', 'packaging\Get-SourceInventory.ps1')) {
        $destination = Join-Path $sourceFixture $relative
        [void][IO.Directory]::CreateDirectory((Split-Path $destination -Parent))
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $destination
    }
    [void][IO.Directory]::CreateDirectory((Join-Path $sourceFixture 'docs'))
    $deletedSource = Join-Path $sourceFixture 'docs\deleted.md'
    [IO.File]::WriteAllText($deletedSource, 'tracked source that will be deleted before packaging')
    & git -C $sourceFixture init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize isolated source-inventory fixture.' }
    & git -C $sourceFixture add -- .
    if ($LASTEXITCODE -ne 0) { throw 'Cannot stage isolated source-inventory fixture.' }
    Remove-Item -LiteralPath $deletedSource
    $newSource = Join-Path $sourceFixture 'docs\new.md'
    [IO.File]::WriteAllText($newSource, 'untracked source must remain in the archive')
    $inventory = & (Join-Path $PSScriptRoot 'Get-SourceInventory.ps1') -Root $sourceFixture
    if ('docs/deleted.md' -in $inventory.Files.Path -or 'docs/new.md' -notin $inventory.Files.Path) {
        throw 'Repository source inventory must omit working-tree deletions and retain new source.'
    }
    $license = Join-Path $sourceFixture 'LICENSE'
    Remove-Item -LiteralPath $license
    Assert-Rejected { & (Join-Path $PSScriptRoot 'Get-SourceInventory.ps1') -Root $sourceFixture } 'Missing required corresponding source: LICENSE'
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $license
    $inventory | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $sourceFixture 'packaging\SourceInventory.json') -Encoding UTF8
    Remove-Item -LiteralPath $newSource
    Assert-Rejected { & (Join-Path $PSScriptRoot 'Get-SourceInventory.ps1') -Root $sourceFixture } 'Missing corresponding source file: docs/new.md'
    [xml]$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Package.appxmanifest')
    $ns = New-Object Xml.XmlNamespaceManager($manifest.NameTable)
    $ns.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
    $ns.AddNamespace('uap', 'http://schemas.microsoft.com/appx/manifest/uap/windows10')
    $ns.AddNamespace('desktop', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10')
    $apps = @($manifest.SelectNodes('/f:Package/f:Applications/f:Application', $ns))
    if ($apps.Count -ne 1 -or $apps[0].Id -ne 'Settings' -or $apps[0].Executable -ne 'WindowSweaters.Settings.exe' -or
        $apps[0].SelectSingleNode('uap:VisualElements', $ns).AppListEntry -ne 'default') {
        throw 'The manifest must expose only visible Settings, with no headless Host application.'
    }
    $extensions = @($apps[0].SelectNodes('f:Extensions/desktop:Extension', $ns))
    if ($extensions.Count -ne 1 -or $extensions[0].Category -ne 'windows.startupTask' -or
        $extensions[0].Executable -ne 'WindowSweaters.Host.exe' -or $extensions[0].EntryPoint -ne 'Windows.FullTrustApplication' -or
        $extensions[0].StartupTask.TaskId -ne 'WindowSweatersStartup' -or $extensions[0].StartupTask.Enabled -ne 'false') {
        throw 'Settings must retain the disabled Host startup extension and stable TaskId.'
    }
    foreach ($platform in @('x64', 'ARM64')) {
        $bytes = New-Object byte[] 256
        $bytes[0] = 0x4d; $bytes[1] = 0x5a
        [Array]::Copy([BitConverter]::GetBytes([uint32]0x80), 0, $bytes, 0x3c, 4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]0x4550), 0, $bytes, 0x80, 4)
        $machine = if ($platform -eq 'ARM64') { [uint16]0xaa64 } else { [uint16]0x8664 }
        [Array]::Copy([BitConverter]::GetBytes($machine), 0, $bytes, 0x84, 2)
        [Array]::Copy([BitConverter]::GetBytes([uint16]0x20b), 0, $bytes, 0x98, 2)
        $path = Join-Path $directory "$platform.exe"
        [IO.File]::WriteAllBytes($path, $bytes)
        $actual = & (Join-Path $PSScriptRoot 'Test-PeArchitecture.ps1') -Path $path -Platform $platform
        if ($actual -ne ('0x{0:X4}' -f $machine)) { throw 'Wrong PE machine result.' }
        $other = if ($platform -eq 'ARM64') { 'x64' } else { 'ARM64' }
        Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-PeArchitecture.ps1') -Path $path -Platform $other } 'Wrong PE machine*'
        $bytes[0x98] = 0x0b; $bytes[0x99] = 0x01
        [IO.File]::WriteAllBytes($path, $bytes)
        Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-PeArchitecture.ps1') -Path $path -Platform $platform } 'Expected native PE32+*'
        [Array]::Copy([BitConverter]::GetBytes([uint32]0xffff), 0, $bytes, 0x3c, 4)
        [IO.File]::WriteAllBytes($path, $bytes)
        Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-PeArchitecture.ps1') -Path $path -Platform $platform } 'Invalid PE header offset*'
    }
    Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -NativeOutputRoot $PSScriptRoot } 'NativeOutputRoot must remain*'
    $root = Split-Path $PSScriptRoot -Parent
    $nativeRoot = Join-Path $root ('out\packaging-contract-' + [Guid]::NewGuid().ToString('N'))
    $native = Join-Path $nativeRoot 'x64\Release'
    [void][IO.Directory]::CreateDirectory($native)
    $inventory = & (Join-Path $PSScriptRoot 'Get-SourceInventory.ps1') -Root $root
    $outputs = @()
    foreach ($name in @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe', 'resources.pri')) {
        $path = Join-Path $native $name
        [IO.File]::WriteAllText($path, 'isolated build-receipt contract fixture')
        $outputs += [ordered]@{ Path = $name; SHA256 = (Get-FileHash -LiteralPath $path).Hash }
    }
    $receipt = [ordered]@{
        SchemaVersion = 1; Platform = 'x64'; Configuration = 'Release'
        NativeOutputRoot = $nativeRoot.Substring($root.Length + 1)
        SourceFiles = $inventory.Files; Outputs = $outputs
    }
    $receiptPath = Join-Path $native 'native-build-report.json'
    $receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    [void](& (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -NativeOutputRoot $nativeRoot)
    [IO.File]::WriteAllText((Join-Path $native 'resources.pri'), 'changed fixture')
    Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -NativeOutputRoot $nativeRoot } 'Native build output changed*'
    $receipt.SourceFiles[0].SHA256 = '0' * 64
    $receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-NativeBuild.ps1') -NativeOutputRoot $nativeRoot } 'Native build source hashes differ*'
    $artifacts = Join-Path $PSScriptRoot 'artifacts\contract-tests\0.1.0.0'
    [void][IO.Directory]::CreateDirectory($artifacts)
    Assert-Rejected { & (Join-Path $PSScriptRoot 'New-Bundle.ps1') -ArtifactDirectory $artifacts -Version '0.1.0.0' -Architectures @('x64', 'x64') } 'Bundle architectures must be unique*'
    Assert-Rejected { & (Join-Path $PSScriptRoot 'New-Bundle.ps1') -ArtifactDirectory $artifacts -Version '0.1.0.0' -Architectures @('x64', 'ARM64') } 'Bundle inputs must contain exactly one*'
    Assert-Rejected { & (Join-Path $PSScriptRoot 'New-Bundle.ps1') -ArtifactDirectory $directory -Version '0.1.0.0' } 'Bundle artifacts must remain*'
    Assert-Rejected { & (Join-Path $PSScriptRoot 'New-Bundle.ps1') -ArtifactDirectory $artifacts -Version '0.2.0.0' } 'Bundle artifacts must use the selected channel/version*'
    Assert-Rejected { & (Join-Path $PSScriptRoot 'New-Bundle.ps1') -ArtifactDirectory $artifacts -Version '..\0.1.0.0' } 'PackageVersion must contain four integers*'
    Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-Package.ps1') -ArtifactDirectory (Join-Path $artifacts 'x64') -Name 'fixture' -Publisher 'fixture' -PublisherDisplayName 'fixture' -Version '0.2.0.0' } 'Package validation must use the selected channel/version/platform*'
} finally {
    if (Test-Path -LiteralPath $sourceFixture) { Remove-Item -LiteralPath $sourceFixture -Recurse -Force }
    if ($nativeRoot) {
        foreach ($name in @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe', 'resources.pri', 'native-build-report.json')) {
            $path = Join-Path $native $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        }
        Remove-Item -LiteralPath $native
        Remove-Item -LiteralPath (Split-Path $native -Parent)
        Remove-Item -LiteralPath $nativeRoot
    }
    foreach ($platform in @('x64', 'ARM64')) {
        $path = Join-Path $directory "$platform.exe"
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
}
& (Join-Path $PSScriptRoot 'Test-PackageBuildBinding.Tests.ps1')
& (Join-Path $PSScriptRoot 'Test-FixtureConfigurations.ps1')
Write-Host 'PASS: source inventory respects repository deletions, retains new source, and rejects missing required/inventoried files; visible Settings-only manifest and disabled Host startup; x64/ARM64 PE machine and PE32+ checks; malformed PE headers; native receipt source/output hashes and path fence; bundle uniqueness, missing architecture, version namespace and output path fences.'
