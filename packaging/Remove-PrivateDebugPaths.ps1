[CmdletBinding()]
param([Parameter(Mandatory)][string]$Path)
$ErrorActionPreference = 'Stop'
$stageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'obj')) + '\'
$fullPath = [IO.Path]::GetFullPath($Path)
if (!$fullPath.StartsWith($stageRoot, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($fullPath) -notin @('WindowSweaters.Host.exe', 'WindowSweaters.Settings.exe')) {
    throw 'Debug-path normalization is restricted to staged Window Sweaters executables.'
}
$bytes = [IO.File]::ReadAllBytes($fullPath)
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
if ([BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b) {
    throw 'Expected a PE32+ native executable.'
}
$optional = $pe + 24
$directories = $optional + 112
if ([BitConverter]::ToUInt32($bytes, $directories + 4 * 8 + 4) -ne 0) {
    throw 'Refusing to alter a signed executable.'
}
$debugRva = [BitConverter]::ToUInt32($bytes, $directories + 6 * 8)
$debugSize = [BitConverter]::ToUInt32($bytes, $directories + 6 * 8 + 4)
$sectionCount = [BitConverter]::ToUInt16($bytes, $pe + 6)
$sectionStart = $optional + [BitConverter]::ToUInt16($bytes, $pe + 20)
$debugOffset = -1
for ($index = 0; $index -lt $sectionCount; $index++) {
    $section = $sectionStart + 40 * $index
    $virtualSize = [BitConverter]::ToUInt32($bytes, $section + 8)
    $virtualAddress = [BitConverter]::ToUInt32($bytes, $section + 12)
    $rawSize = [BitConverter]::ToUInt32($bytes, $section + 16)
    if ($debugRva -ge $virtualAddress -and $debugRva -lt $virtualAddress + [Math]::Max($virtualSize, $rawSize)) {
        $debugOffset = [BitConverter]::ToUInt32($bytes, $section + 20) + $debugRva - $virtualAddress
        break
    }
}
$changed = $false
if ($debugSize -and ($debugOffset -lt 0 -or $debugOffset + $debugSize -gt $bytes.Length -or $debugSize % 28)) {
    throw 'Invalid PE debug directory.'
}
for ($offset = $debugOffset; $debugSize -and $offset -lt $debugOffset + $debugSize; $offset += 28) {
    if ([BitConverter]::ToUInt32($bytes, $offset + 12) -ne 2) { continue }
    $size = [BitConverter]::ToUInt32($bytes, $offset + 16)
    $pointer = [BitConverter]::ToUInt32($bytes, $offset + 24)
    if ($size -lt 25 -or $pointer + $size -gt $bytes.Length -or [Text.Encoding]::ASCII.GetString($bytes, $pointer, 4) -ne 'RSDS') {
        throw 'Unsupported or invalid native CodeView debug record.'
    }
    $pathOffset = $pointer + 24
    $pathLength = $size - 24
    $pdb = [Text.Encoding]::UTF8.GetString($bytes, $pathOffset, $pathLength).TrimEnd([char]0)
    $basename = [IO.Path]::GetFileName($pdb)
    if ($basename -ne $pdb) {
        $replacement = [Text.Encoding]::UTF8.GetBytes($basename)
        if ($replacement.Length -ge $pathLength) { throw 'PDB basename does not fit CodeView record.' }
        [Array]::Clear($bytes, $pathOffset, $pathLength)
        [Array]::Copy($replacement, 0, $bytes, $pathOffset, $replacement.Length)
        $changed = $true
    }
}
if ($changed) {
    # Zeroing is valid: ordinary EXEs do not require a PE checksum.
    [Array]::Clear($bytes, $optional + 64, 4)
    [IO.File]::WriteAllBytes($fullPath, $bytes)
}
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
$utf16 = [Text.Encoding]::Unicode.GetString($bytes)
if ($ascii -match '(?i)[A-Z]:\\Users\\|/Users/|/home/' -or $utf16 -match '(?i)[A-Z]:\\Users\\|/Users/|/home/') {
    throw 'Staged executable still contains a private home path. Build native Release with portable PDB paths and WINRT_NO_SOURCE_LOCATION.'
}
