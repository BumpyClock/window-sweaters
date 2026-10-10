[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Root,
    [switch]$ValidateHashes
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\')
$inventoryRelative = 'packaging/SourceInventory.json'
function Test-ExcludedSource([string]$relative) {
    return $relative -match '(?i)(^|/)(\.git|\.vs|\.packages|obj|bin|out|artifacts|outputs|probe-results)/' -or
        $relative -match '(?i)\.(user|pdb|ilk|log|pfx|pem|key|msix|msixbundle|zip)$'
}
function Test-SourcePath([string]$relative) {
    if ($relative -match '(^|/)\.\.?(/|$)|[\\:<>|?*\x00-\x1f]' -or $relative.StartsWith('/') -or
        $relative.EndsWith('/') -or $relative.Contains('//') -or (Test-ExcludedSource $relative)) { return $false }
    return $relative -match '^(src|tests|scripts|packaging|docs|\.github)/' -or
        $relative -match '^(LICENSE|NOTICE\.md|README\.md|Directory\.Build\.props|WindowSweaters\.sln|\.gitignore)$'
}
$archiveInventory = Join-Path $rootPath $inventoryRelative.Replace('/', '\')
$expected = @{}
if (Test-Path -LiteralPath $archiveInventory -PathType Leaf) {
    $inventory = Get-Content -LiteralPath $archiveInventory -Raw | ConvertFrom-Json
    if ($inventory.SchemaVersion -ne 1 -or !$inventory.Files.Count) { throw 'Invalid corresponding-source inventory format.' }
    $paths = @($inventory.Files | ForEach-Object {
        if (!(Test-SourcePath $_.Path) -or $_.Path -eq $inventoryRelative -or $expected.ContainsKey($_.Path) -or
            $_.SHA256 -notmatch '^[A-Fa-f0-9]{64}$') { throw "Invalid/duplicate source inventory entry: $($_.Path)" }
        $expected[$_.Path] = $_.SHA256
        $_.Path
    })
    $directories = New-Object 'Collections.Generic.Queue[string]'
    $directories.Enqueue($rootPath)
    while ($directories.Count) {
        $directory = $directories.Dequeue()
        foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
            $relative = $item.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
            $testPath = $relative
            if ($item.PSIsContainer) { $testPath += '/' }
            if (Test-ExcludedSource $testPath) { continue }
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source archive contains a reparse point: $relative" }
            if ($item.PSIsContainer) { $directories.Enqueue($item.FullName); continue }
            if ($relative -ne $inventoryRelative -and !$expected.ContainsKey($relative)) {
                throw "Unexpected file not declared in source inventory: $relative"
            }
        }
    }
} elseif (Test-Path -LiteralPath (Join-Path $rootPath '.git')) {
    $listed = & git -C $rootPath ls-files --cached --others --exclude-standard
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate repository corresponding source.' }
    $paths = @($listed | Where-Object { $_ -ne $inventoryRelative -and (Test-SourcePath $_) } | Sort-Object -Unique)
} else {
    throw 'Corresponding source requires repository .git metadata or packaging\SourceInventory.json from the distributed source archive.'
}
$files = @()
foreach ($relative in @($paths | Sort-Object)) {
    if (!(Test-SourcePath $relative)) { throw "Unsafe source inventory path: $relative" }
    $path = Join-Path $rootPath $relative.Replace('/', '\')
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing corresponding source file: $relative" }
    $item = Get-Item -LiteralPath $path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source file is a reparse point: $relative" }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($ValidateHashes -and $expected.ContainsKey($relative) -and $expected[$relative] -ne $hash) {
        throw "Source archive hash mismatch: $relative"
    }
    $files += [ordered]@{ Path = $relative; SHA256 = $hash }
}
foreach ($required in @('LICENSE', 'NOTICE.md', 'WindowSweaters.sln', 'src/Host/WindowSweaters.Host.vcxproj',
    'src/Settings/WindowSweaters.Settings.vcxproj', 'scripts/build.ps1', 'scripts/package.ps1',
    'packaging/Package.appxmanifest', 'packaging/WindowSweaters.Package.wapproj', 'packaging/Get-SourceInventory.ps1')) {
    if ($required -notin $files.Path) { throw "Missing required corresponding source: $required" }
}
[pscustomobject][ordered]@{ SchemaVersion = 1; Files = $files }
