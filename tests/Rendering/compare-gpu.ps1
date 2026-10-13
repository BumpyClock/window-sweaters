[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [switch]$NoBuild,
    [switch]$SkipWarp,
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildRoot = Join-Path $root 'out\gpu-validation'
$output = Join-Path $buildRoot "$Platform\$Configuration"
$artifacts = Join-Path $root "out\gpu-comparison\$Platform-$Configuration"
Push-Location $root
try {
    if (!$NoBuild) {
        foreach ($project in @('tests\IconYarn\IconYarnTests.vcxproj',
            'src\Host\WindowSweaters.Host.vcxproj', 'tests\Rendering\GpuShaderTests.vcxproj',
            'tests\Rendering\RenderQueueTests.vcxproj', 'tests\Rendering\GraphicsRecoveryTests.vcxproj',
            'tests\Knitting\WindowSweaters.Knitting.Tests.vcxproj')) {
            & $MSBuild $project /nologo /m /verbosity:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform" `
                "/p:NativeOutputRoot=$buildRoot" "/p:NativeIntermediateRoot=$buildRoot\obj"
            if ($LASTEXITCODE) { throw "Build failed: $project ($LASTEXITCODE)" }
        }
    }
    if ($Platform -eq 'ARM64' -and [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'Arm64') {
        Write-Output 'BUILT ONLY: ARM64 execution requires an ARM64 Windows host.'
        return
    }
    New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
    $head = & git rev-parse HEAD
    if ($LASTEXITCODE) { throw "git rev-parse failed ($LASTEXITCODE)" }
    $branch = & git branch --show-current
    if ($LASTEXITCODE) { throw "git branch failed ($LASTEXITCODE)" }
    $status = @(& git --no-pager status --short)
    if ($LASTEXITCODE) { throw "git status failed ($LASTEXITCODE)" }
    $source = [ordered]@{
        head = $head
        branch = $branch
        status = $status
        configuration = $Configuration
        platform = $Platform
        sdk = '10.0.26100.0'
        toolset = 'v145'
        recordedUtc = [DateTime]::UtcNow.ToString('o')
        displayDrivers = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion)
        sourceHashes = @(
            Get-ChildItem src\Rendering, src\Knitting, tests\Rendering -File |
                Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash
            Get-Item src\Host\main.cpp, src\Host\RenderState.h, src\Host\HostContractTests.cpp,
                src\Host\WindowSweaters.Host.vcxproj |
                Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash
        )
        binaryHashes = @(Get-Item (Join-Path $output 'WindowSweaters.Host.exe'), (Join-Path $output 'GpuShaderTests.exe') |
            Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash)
    }
    $source | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $artifacts 'source.json') -Encoding utf8
    foreach ($exe in @('RenderQueueTests.exe', 'GraphicsRecoveryTests.exe', 'WindowSweaters.Knitting.Tests.exe')) {
        & (Join-Path $output $exe)
        if ($LASTEXITCODE) { throw "Test failed: $exe ($LASTEXITCODE)" }
    }
    $process = Start-Process (Join-Path $output 'WindowSweaters.Host.exe') -WindowStyle Hidden `
        -ArgumentList '--integration-contract' -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $artifacts 'gpu-host.stdout.log') `
        -RedirectStandardError (Join-Path $artifacts 'gpu-host.stderr.log')
    if ($process.ExitCode) { throw "Hidden Host GPU contract failed ($($process.ExitCode))" }
    foreach ($arguments in @('--renderer', '--renderer cpu', '--renderer gpu', '--renderer invalid', '--renderer cpu --renderer gpu',
        '--integration-contract --renderer invalid', '--integration-contract --test-mode',
        '--integration-contract --target 1')) {
        $process = Start-Process (Join-Path $output 'WindowSweaters.Host.exe') -WindowStyle Hidden -ArgumentList $arguments -PassThru -Wait
        if ($process.ExitCode -ne 2) { throw "CLI rejection failed: $arguments ($($process.ExitCode))" }
    }
    $comparison = Join-Path $output 'GpuShaderTests.exe'
    & $comparison --output (Join-Path $artifacts 'hardware')
    if ($LASTEXITCODE) { throw "Hardware GPU comparison failed ($LASTEXITCODE)" }
    if (!$SkipWarp) {
        & $comparison --warp --output (Join-Path $artifacts 'warp')
        if ($LASTEXITCODE) { throw "WARP GPU comparison failed ($LASTEXITCODE)" }
    }
    foreach ($file in @('generation-benchmark.csv', 'benchmark.csv')) {
        $summary = Import-Csv (Join-Path $artifacts "hardware\$file") | Group-Object theme,renderer,cache |
            ForEach-Object {
                $times = @($_.Group | ForEach-Object { [double]$_.total_ms } | Sort-Object)
                $middle = [int][Math]::Floor($times.Count / 2)
                $median = if ($times.Count % 2) { $times[$middle] } else {
                    ($times[$middle - 1] + $times[$middle]) / 2
                }
                [PSCustomObject]@{
                    case = $_.Name
                    samples = $times.Count
                    mean_ms = ($times | Measure-Object -Average).Average
                    median_ms = $median
                    min_ms = $times[0]
                    max_ms = $times[-1]
                }
            }
        $summary | Export-Csv (Join-Path $artifacts ($file.Replace('.csv', '-summary.csv'))) -NoTypeInformation
        $summary | Format-Table -AutoSize
    }
    $backends = if ($SkipWarp) { 'hardware (WARP skipped)' } else { 'hardware/WARP' }
    Write-Output "PASS: GPU hidden contracts, $backends shader parity and completed-GPU benchmarks. Artifacts: $artifacts"
} finally {
    Pop-Location
}
