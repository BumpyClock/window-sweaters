[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][ValidateSet('x64', 'ARM64')][string]$Platform
)
$ErrorActionPreference = 'Stop'
$stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Path))
$reader = [IO.BinaryReader]::new($stream)
try {
    if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw "Not a PE binary: $Path" }
    $stream.Position = 0x3c
    $offset = $reader.ReadUInt32()
    if ($offset -gt $stream.Length - 26) { throw "Invalid PE header offset: $Path" }
    $stream.Position = $offset
    if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $Path" }
    $machine = $reader.ReadUInt16()
    $expected = if ($Platform -eq 'ARM64') { 0xaa64 } else { 0x8664 }
    if ($machine -ne $expected) {
        throw ("Wrong PE machine for {0}: expected 0x{1:X4}, found 0x{2:X4}: {3}" -f $Platform, $expected, $machine, $Path)
    }
    $stream.Position = $offset + 24
    if ($reader.ReadUInt16() -ne 0x20b) { throw "Expected native PE32+: $Path" }
    '0x{0:X4}' -f $machine
} finally { $reader.Dispose(); $stream.Dispose() }
