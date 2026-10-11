[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$directory = Join-Path $PSScriptRoot 'obj\contract-tests'
$nativeRoot = $null
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
    Assert-Rejected { & (Join-Path $PSScriptRoot 'Test-Package.ps1') -ArtifactDirectory (Join-Path $artifacts 'x64') -Name 'fixture' -Publisher 'fixture' -Version '0.2.0.0' } 'Package validation must use the selected channel/version/platform*'
} finally {
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
Write-Host 'PASS: x64/ARM64 PE machine and PE32+ checks; malformed PE headers; native receipt source/output hashes and path fence; bundle uniqueness, missing architecture, version namespace and output path fences.'
