[CmdletBinding()]
param(
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!(Test-Path -LiteralPath $MSBuild -PathType Leaf)) { throw "MSBuild not found: $MSBuild" }
$project = Join-Path $PSScriptRoot 'WindowSweaters.Package.wapproj'
foreach ($platform in @('x64', 'ARM64')) {
    foreach ($mode in @('Always', 'Never')) {
        $json = & $MSBuild $project /nologo "/p:Platform=$platform" "/p:AppxBundle=$mode" `
            '-getProperty:AppxBundle,_CreateAppxBundlePlatformSpecificArtifactsDependsOn,AppxGeneratePriEnabled,IncludeProjectPriFile'
        if ($LASTEXITCODE -ne 0) { throw "MSBuild bundle property evaluation failed: $LASTEXITCODE" }
        $properties = ($json -join [Environment]::NewLine | ConvertFrom-Json).Properties
        if ($properties.AppxBundle -ne $mode -or $properties.AppxGeneratePriEnabled -ne 'false' -or
            $properties.IncludeProjectPriFile -ne 'false') {
            throw "Incorrect $platform/$mode bundle or PRI properties."
        }
        $route = $properties._CreateAppxBundlePlatformSpecificArtifactsDependsOn.Trim()
        if ($mode -eq 'Always' -and $route -ne 'BuildWindowSweatersBundle') {
            throw "Visual Studio $platform bundle does not use the validated bundler."
        }
        if ($mode -eq 'Never' -and $route -eq 'BuildWindowSweatersBundle') {
            throw "Single-platform $platform MSIX unexpectedly routes to the bundler."
        }
        & $MSBuild $project /nologo /v:quiet /t:ValidateWindowSweatersPackage "/p:Platform=$platform" "/p:AppxBundle=$mode"
        if ($LASTEXITCODE -ne 0) { throw "Valid $platform/$mode packaging request was rejected." }
    }
}
function Assert-MSBuildRejected([string[]]$Properties, [string]$Expected) {
    $output = & $MSBuild $project /nologo /v:quiet /t:ValidateWindowSweatersPackage @Properties 2>&1
    if ($LASTEXITCODE -eq 0 -or ($output -join [Environment]::NewLine) -notlike "*$Expected*") {
        throw "Expected MSBuild rejection: $Expected. Output: $($output -join [Environment]::NewLine)"
    }
}
$directory = Join-Path $PSScriptRoot ('obj\store-contract-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($directory)
$props = Join-Path $directory 'identity.props.user'
try {
    [IO.File]::WriteAllText($props, @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <PackageName>WindowSweaters.StoreContractFixture</PackageName>
    <PackagePublisher>CN=StoreContractFixture</PackagePublisher>
    <PackagePublisherDisplayName>Weakly Design</PackagePublisherDisplayName>
  </PropertyGroup>
</Project>
'@)
    foreach ($platform in @('x64', 'ARM64')) {
        foreach ($mode in @('StoreUpload', 'StoreAndSideload', 'StoreOnly')) {
            $arguments = @("/p:Platform=$platform", "/p:UapAppxPackageBuildMode=$mode", "/p:StoreIdentityProps=$props")
            $json = & $MSBuild $project /nologo @arguments `
                '-getProperty:PackageChannel,PackageName,PackagePublisher,PackagePublisherDisplayName,PackageDisplayName,PackageStage'
            if ($LASTEXITCODE -ne 0) { throw "MSBuild Store property evaluation failed: $LASTEXITCODE" }
            $properties = ($json -join [Environment]::NewLine | ConvertFrom-Json).Properties
            if ($properties.PackageChannel -ne 'Store' -or $properties.PackageName -ne 'WindowSweaters.StoreContractFixture' -or
                $properties.PackagePublisher -ne 'CN=StoreContractFixture' -or
                $properties.PackagePublisherDisplayName -cne 'Weakly Design' -or
                $properties.PackageDisplayName -ne 'Window Sweaters' -or $properties.PackageStage -notlike "*\obj\Store\0.1.0.0\$platform\Release\stage\") {
                throw "Store publishing must select Store identity and staging: $platform/$mode."
            }
            & $MSBuild $project /nologo /v:quiet /t:ValidateWindowSweatersPackage @arguments
            if ($LASTEXITCODE -ne 0) { throw "Valid $platform/$mode Store request was rejected." }
            Assert-MSBuildRejected ($arguments + '/p:PackageChannel=Development') 'Store publishing cannot use PackageChannel=Development'
        }
        Assert-MSBuildRejected @("/p:Platform=$platform", '/p:UapAppxPackageBuildMode=StoreUpload', "/p:StoreIdentityProps=$directory\missing.props.user") 'Store packaging requires a local StoreIdentityProps'
    }
} finally {
    Remove-Item -LiteralPath $props -Force
    Remove-Item -LiteralPath $directory
}
foreach ($platforms in @('', 'x86', 'x64|x86', 'x64||ARM64', 'x64|x64', 'arm64|ARM64')) {
    $rejected = $false
    try {
        & (Join-Path $PSScriptRoot 'Build-Bundle.ps1') -Platforms $platforms -MSBuild $MSBuild `
            -PackageInfoPath (Join-Path $PSScriptRoot 'obj\invalid-bundle-pkginfo.txt') | Out-Null
    } catch {
        if ($_.Exception.Message -notmatch 'AppxBundlePlatforms|empty string') { throw }
        $rejected = $true
    }
    if (!$rejected) { throw "Expected invalid bundle platform rejection: $platforms" }
}
Write-Host 'PASS: x64/ARM64 Visual Studio bundle routing, preserved PRI settings, single-MSIX requests, Store publishing identity selection, Development/Store mismatch and missing identity rejection, and invalid/duplicate architecture rejection.'
