<#
.SYNOPSIS
Collects read-only evidence from a fixture started by the computer-use client.

.DESCRIPTION
The fixture PID must be supplied explicitly. This script never starts, stops,
focuses, moves, clicks, or sends input to a window. The fixture's own buttons
launch and stop only the sibling test-mode host. Use -WaitForHostExit while
stopping that host through the visible fixture UI.

.PARAMETER FixturePid
PID shown by the computer-use client for the launched fixture process.

.PARAMETER WaitForHostExit
Waits while the user stops the fixture-owned host through the visible Stop
test host button. The script only observes the process and window handles.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateRange(1, 2147483647)]
    [int]$FixturePid,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$WaitForHostExit,
    [ValidateRange(1, 300)]
    [int]$WaitTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not ('WindowSweaters.Probe.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace WindowSweaters.Probe
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Point
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    public static class Native
    {
        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool GetWindowRect(IntPtr window, out Rect rect);

        [DllImport("user32.dll")]
        public static extern IntPtr WindowFromPoint(Point point);

        [DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

        [DllImport("user32.dll")]
        public static extern bool IsWindow(IntPtr window);

        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr window);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern int GetClassNameW(IntPtr window, StringBuilder className, int maximumCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern int GetWindowTextW(IntPtr window, StringBuilder text, int maximumCount);

        [DllImport("user32.dll")]
        private static extern IntPtr GetDC(IntPtr window);

        [DllImport("user32.dll")]
        private static extern int ReleaseDC(IntPtr window, IntPtr deviceContext);

        [DllImport("gdi32.dll")]
        private static extern uint GetPixel(IntPtr deviceContext, int x, int y);

        public static uint GetProcessId(IntPtr window)
        {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            return processId;
        }

        public static string GetClass(IntPtr window)
        {
            StringBuilder value = new StringBuilder(256);
            GetClassNameW(window, value, value.Capacity);
            return value.ToString();
        }

        public static string GetTitle(IntPtr window)
        {
            StringBuilder value = new StringBuilder(512);
            GetWindowTextW(window, value, value.Capacity);
            return value.ToString();
        }

        public static uint GetScreenPixel(int x, int y)
        {
            IntPtr dc = GetDC(IntPtr.Zero);
            if (dc == IntPtr.Zero)
            {
                throw new InvalidOperationException("GetDC(NULL) failed.");
            }
            try
            {
                uint color = GetPixel(dc, x, y);
                if (color == 0xffffffff)
                {
                    throw new InvalidOperationException("GetPixel failed.");
                }
                return color;
            }
            finally
            {
                ReleaseDC(IntPtr.Zero, dc);
            }
        }
    }
}
'@
}

$root = Split-Path $PSScriptRoot -Parent
$outputDirectory = Join-Path $root "out\x64\$Configuration"
$fixtureExecutable = Join-Path $outputDirectory 'Fixture.exe'
$probeDirectory = Join-Path $outputDirectory 'probe-results'
if (-not (Test-Path -LiteralPath $fixtureExecutable)) {
    throw "Fixture executable not found: $fixtureExecutable. Build the fixture before using Computer Use."
}
if (-not (Test-Path -LiteralPath $probeDirectory)) {
    throw "Fixture evidence directory not found: $probeDirectory."
}

function ConvertTo-Hwnd {
    param([Parameter(Mandatory)]$Value)
    if ($Value -is [string]) {
        $text = [string]$Value
        if ($text.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
            return [IntPtr]::new([Convert]::ToInt64($text.Substring(2), 16))
        }
        return [IntPtr]::new([Convert]::ToInt64($text, 10))
    }
    return [IntPtr]::new([Convert]::ToInt64($Value))
}

function Read-Ndjson {
    param([Parameter(Mandatory)][string]$Path)
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue) {
        if (-not $line) { continue }
        try { $records.Add(($line | ConvertFrom-Json -ErrorAction Stop)) } catch {}
    }
    return ,$records.ToArray()
}

function Get-RectArray {
    param([Parameter(Mandatory)][IntPtr]$Window)
    $rect = [WindowSweaters.Probe.Rect]::new()
    if (-not [WindowSweaters.Probe.Native]::GetWindowRect($Window, [ref]$rect)) {
        throw "GetWindowRect failed for HWND $Window."
    }
    return ,@($rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
}

function Get-PointArray {
    param([Parameter(Mandatory)]$Value)
    if ($Value.Count -ne 2) { throw "Expected a two-coordinate point, received '$Value'." }
    return ,@([int]$Value[0], [int]$Value[1])
}

function Get-PointOwner {
    param([Parameter(Mandatory)][int]$X, [Parameter(Mandatory)][int]$Y)
    $point = [WindowSweaters.Probe.Point]::new()
    $point.X = $X
    $point.Y = $Y
    $window = [WindowSweaters.Probe.Native]::WindowFromPoint($point)
    return [pscustomobject]@{
        hwnd = '0x{0:x}' -f $window.ToInt64()
        window = $window
        pid = [uint32][WindowSweaters.Probe.Native]::GetProcessId($window)
        class = [WindowSweaters.Probe.Native]::GetClass($window)
    }
}

function Get-Rgb {
    param([uint32]$ColorRef)
    return ,@(
        [int]($ColorRef -band 0xff),
        [int](($ColorRef -shr 8) -band 0xff),
        [int](($ColorRef -shr 16) -band 0xff)
    )
}

function Get-RgbDifference {
    param([object[]]$Actual, [object[]]$Expected)
    return ,@(
        [Math]::Abs([int]$Actual[0] - [int]$Expected[0]),
        [Math]::Abs([int]$Actual[1] - [int]$Expected[1]),
        [Math]::Abs([int]$Actual[2] - [int]$Expected[2])
    )
}

function Test-ContainsPoint {
    param([object[]]$Bounds, [int]$X, [int]$Y)
    return $Bounds.Count -eq 4 -and $X -ge $Bounds[0] -and $X -lt $Bounds[2] -and
        $Y -ge $Bounds[1] -and $Y -lt $Bounds[3]
}

function Get-ProcessPath {
    param([Parameter(Mandatory)][System.Diagnostics.Process]$Process)
    try { return [IO.Path]::GetFullPath($Process.MainModule.FileName) }
    catch { throw "Could not verify executable path for PID $($Process.Id): $($_.Exception.Message)" }
}

function Assert-OwnedWindow {
    param(
        [Parameter(Mandatory)][IntPtr]$Window,
        [Parameter(Mandatory)][uint32]$ExpectedPid,
        [Parameter(Mandatory)][string]$Description
    )
    if ($Window -eq [IntPtr]::Zero -or -not [WindowSweaters.Probe.Native]::IsWindow($Window)) {
        throw "$Description is not a live window."
    }
    $owner = [uint32][WindowSweaters.Probe.Native]::GetProcessId($Window)
    if ($owner -ne $ExpectedPid) {
        throw "$Description belongs to PID $owner, not the expected owned PID $ExpectedPid."
    }
}

function Get-MessageCount {
    param([object[]]$Records, [string]$Role, [string]$Message)
    return @($Records | Where-Object {
        $_.type -eq 'window.message' -and $_.role -eq $Role -and $_.message -eq $Message
    }).Count
}

$fixtureProcess = $null
$fixtureReadyProcess = $null
$fixtureReady = $null
$fixtureRecordsPath = $null
$hostProcess = $null
$hostProbe = $null
$hostWindow = [IntPtr]::Zero
$overlayWindow = [IntPtr]::Zero
$fixtureRecords = @()
$pixelEvidence = [ordered]@{}
$shutdownEvidence = 'not-requested; stop the host through the fixture UI'
$failure = $null
$summaryPath = Join-Path $probeDirectory "fixture.$FixturePid.observation.json"

try {
    $candidates = @(Get-ChildItem -LiteralPath $probeDirectory -File -Filter "fixture.$FixturePid.*.ndjson" |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($candidates.Count -eq 0) {
        throw "No fixture evidence file exists for PID $FixturePid under $probeDirectory."
    }
    $fixtureRecordsPath = $candidates[0].FullName
    $fixtureRecords = Read-Ndjson -Path $fixtureRecordsPath
    $fixtureReady = $fixtureRecords | Where-Object type -eq 'fixture.ready' | Select-Object -First 1
    if (-not $fixtureReady) {
        throw "Evidence file has no fixture.ready record: $fixtureRecordsPath."
    }
    $controlsReady = $fixtureRecords | Where-Object type -eq 'fixture.controls_ready' | Select-Object -First 1
    if (-not $controlsReady) {
        throw "Fixture PID $FixturePid did not report creating the accessible test controls; rebuild the fixture binary."
    }
    if ([uint32]$fixtureReady.pid -ne [uint32]$FixturePid) {
        throw "fixture.ready PID $($fixtureReady.pid) does not match explicitly supplied PID $FixturePid."
    }
    if ([uint32]$controlsReady.pid -ne [uint32]$FixturePid -or
        (ConvertTo-Hwnd $controlsReady.target_hwnd) -ne (ConvertTo-Hwnd $fixtureReady.target_hwnd)) {
        throw 'The fixture control-creation record belongs to a different process or target HWND.'
    }
    if ($fixtureReady.target_class -ne 'WindowSweaters.TestFixture' -or
        $fixtureReady.target_title -ne 'WindowSweaters Fixture' -or
        $fixtureReady.dpi_awareness -ne 'per-monitor-v2') {
        throw 'Fixture class/title/DPI awareness does not match the test fixture contract.'
    }
    if (-not (Test-Path -LiteralPath $fixtureReady.result_file) -or
        [IO.Path]::GetFullPath($fixtureReady.result_file) -ne [IO.Path]::GetFullPath($fixtureRecordsPath)) {
        throw 'The fixture.ready result_file does not match its captured evidence file.'
    }

    $fixtureTarget = ConvertTo-Hwnd $fixtureReady.target_hwnd
    $fixtureReceiver = ConvertTo-Hwnd $fixtureReady.receiver_hwnd
    $expectedFixturePath = [IO.Path]::GetFullPath($fixtureExecutable)
    $fixtureReadyProcess = $null
    try { $fixtureReadyProcess = [System.Diagnostics.Process]::GetProcessById($FixturePid) } catch {}
    if ($fixtureReadyProcess) {
        if ((Get-ProcessPath $fixtureReadyProcess) -ne $expectedFixturePath) {
            throw "PID $FixturePid is not the fixture built at $expectedFixturePath."
        }
        Assert-OwnedWindow -Window $fixtureTarget -ExpectedPid ([uint32]$FixturePid) -Description 'Fixture target'
        Assert-OwnedWindow -Window $fixtureReceiver -ExpectedPid ([uint32]$FixturePid) -Description 'Fixture hit receiver'
        if ([WindowSweaters.Probe.Native]::GetClass($fixtureReceiver) -ne 'WindowSweaters.TestFixtureReceiver' -or
            [WindowSweaters.Probe.Native]::GetTitle($fixtureReceiver) -ne 'WindowSweaters Fixture Receiver') {
            throw 'The exterior hit receiver class/title do not match the fixture contract.'
        }
        $controls = @(
            @{ name = 'start_host_hwnd'; title = 'Start test host' },
            @{ name = 'stop_host_hwnd'; title = 'Stop test host' },
            @{ name = 'keyboard_input_hwnd'; title = 'Click here, then type a short test string.' },
            @{ name = 'scroll_list_hwnd'; title = '' },
            @{ name = 'status_hwnd'; title = '' }
        )
        foreach ($control in $controls) {
            $controlValue = $controlsReady.PSObject.Properties[$control.name].Value
            $controlWindow = ConvertTo-Hwnd $controlValue
            Assert-OwnedWindow -Window $controlWindow -ExpectedPid ([uint32]$FixturePid) -Description $control.name
            if (-not [WindowSweaters.Probe.Native]::IsWindowVisible($controlWindow)) {
                throw "$($control.name) exists but is not visible."
            }
            if ($control.title -and [WindowSweaters.Probe.Native]::GetTitle($controlWindow) -ne $control.title) {
                throw "$($control.name) has an unexpected accessible title."
            }
        }
        if (-not [WindowSweaters.Probe.Native]::IsWindowVisible($fixtureTarget)) {
            throw 'Fixture target exists but is not visible; screen pixel evidence is unavailable.'
        }
    }

    $fixtureState = if ($fixtureReadyProcess) { 'running' } else { 'exited; retained NDJSON evidence only' }
    if ($fixtureReadyProcess) {
        $targetBounds = Get-RectArray -Window $fixtureTarget
        $receiverBounds = Get-RectArray -Window $fixtureReceiver
        $center = Get-PointArray $fixtureReady.points.target_center
        $centerOwner = Get-PointOwner -X $center[0] -Y $center[1]
        if ($centerOwner.pid -eq [uint32]$FixturePid) {
            $centerPixel = [WindowSweaters.Probe.Native]::GetScreenPixel($center[0], $center[1])
            $centerRgb = Get-Rgb -ColorRef $centerPixel
            $centerDelta = Get-RgbDifference -Actual $centerRgb -Expected $fixtureReady.expected_target_rgb
            $pixelEvidence.transparent_center = [ordered]@{
                sample = $center
                window_from_point = $centerOwner.hwnd
                window_from_point_class = $centerOwner.class
                window_from_point_pid = $centerOwner.pid
                rgb = $centerRgb
                expected_fixture_rgb = @($fixtureReady.expected_target_rgb)
                delta_rgb = $centerDelta
                matches_expected_fixture_background = (@($centerDelta | Where-Object { $_ -gt 3 }).Count -eq 0)
            }
        } else {
            $pixelEvidence.transparent_center = "not-sampled; WindowFromPoint belongs to PID $($centerOwner.pid)"
        }
    } else {
        $pixelEvidence.transparent_center = 'not-sampled; fixture process is no longer running'
    }

    $hostProbePath = [string]$fixtureReady.host_probe_file
    if (-not $hostProbePath -or -not (Test-Path -LiteralPath $hostProbePath)) {
        $hostState = if ($fixtureReadyProcess) { 'not-started or not-ready' } else { 'no saved host probe' }
        $shutdownEvidence = 'not-observed; the fixture has no host readiness probe'
    } else {
        $hostProbe = Get-Content -LiteralPath $hostProbePath -Raw | ConvertFrom-Json
        $hostPid = [uint32]$hostProbe.host_pid
        $expectedHostPath = Join-Path $outputDirectory 'WindowSweaters.Host.exe'
        $hostState = 'probe-written'
        $hostWindow = ConvertTo-Hwnd $hostProbe.host_hwnd
        $overlayWindow = ConvertTo-Hwnd $hostProbe.overlay_hwnd
        $reportedTarget = ConvertTo-Hwnd $hostProbe.target_hwnd
        if ($reportedTarget -ne $fixtureTarget) {
            throw 'The host probe target HWND differs from this fixture target.'
        }
        try { $hostProcess = [System.Diagnostics.Process]::GetProcessById([int]$hostPid) } catch {}

        if ($hostProcess) {
            if ((Get-ProcessPath $hostProcess) -ne [IO.Path]::GetFullPath($expectedHostPath)) {
                throw "Fixture reports host PID $hostPid, but it is not the sibling test host $expectedHostPath."
            }
            Assert-OwnedWindow -Window $hostWindow -ExpectedPid $hostPid -Description 'Host control window'
            Assert-OwnedWindow -Window $overlayWindow -ExpectedPid $hostPid -Description 'Overlay window'
            if ([WindowSweaters.Probe.Native]::GetClass($hostWindow) -ne 'WindowSweaters.Host' -or
                [WindowSweaters.Probe.Native]::GetTitle($hostWindow) -ne 'Window Sweaters' -or
                [WindowSweaters.Probe.Native]::GetClass($overlayWindow) -ne 'WindowSweaters.Overlay') {
                throw 'Host/overlay class or title does not match the fixture contract.'
            }

            if ($fixtureReadyProcess) {
                $targetBounds = Get-RectArray -Window $fixtureTarget
                $overlayBounds = @($hostProbe.overlay_bounds | ForEach-Object { [int]$_ })
                $center = Get-PointArray $fixtureReady.points.target_center
                $ringX = [int](($targetBounds[0] + $targetBounds[2]) / 2)
                if ($overlayBounds.Count -ne 4 -or
                    -not (Test-ContainsPoint -Bounds $overlayBounds -X $ringX -Y $overlayBounds[1]) -or
                    -not (Test-ContainsPoint -Bounds $receiverBounds -X $ringX -Y $overlayBounds[1])) {
                    throw 'The exterior sample is not inside both the overlay and fixture-only receiver.'
                }

                $ringSamples = [System.Collections.Generic.List[object]]::new()
                for ($sampleY = [int]$overlayBounds[1]; $sampleY -lt [int]$targetBounds[1]; $sampleY++) {
                    if (-not (Test-ContainsPoint -Bounds $overlayBounds -X $ringX -Y $sampleY) -or
                        -not (Test-ContainsPoint -Bounds $receiverBounds -X $ringX -Y $sampleY)) {
                        continue
                    }
                    $ringOwner = Get-PointOwner -X $ringX -Y $sampleY
                    if ($ringOwner.pid -ne [uint32]$FixturePid -or $ringOwner.window -ne $fixtureReceiver) {
                        continue
                    }
                    $ringPixel = [WindowSweaters.Probe.Native]::GetScreenPixel($ringX, $sampleY)
                    $ringRgb = Get-Rgb -ColorRef $ringPixel
                    $ringDelta = Get-RgbDifference -Actual $ringRgb -Expected $hostProbe.color_rgb
                    $ringSamples.Add([ordered]@{
                        sample = @($ringX, $sampleY)
                        window_from_point = $ringOwner.hwnd
                        window_from_point_class = $ringOwner.class
                        window_from_point_pid = $ringOwner.pid
                        rgb = $ringRgb
                        expected_host_rgb = @($hostProbe.color_rgb)
                        delta_rgb = $ringDelta
                    })
                }
                $receiverPoint = [WindowSweaters.Probe.Point]@{
                    X = [int]$receiverBounds[0] + 8
                    Y = [int]$receiverBounds[1] + 8
                }
                $receiverOwner = Get-PointOwner -X $receiverPoint.X -Y $receiverPoint.Y
                $receiverRgb = $null
                if ($receiverOwner.pid -eq [uint32]$FixturePid -and $receiverOwner.window -eq $fixtureReceiver) {
                    $receiverPixel = [WindowSweaters.Probe.Native]::GetScreenPixel($receiverPoint.X, $receiverPoint.Y)
                    $receiverRgb = Get-Rgb -ColorRef $receiverPixel
                }
                $closestSample = $ringSamples | Sort-Object {
                    ($_.delta_rgb | ForEach-Object { [int]$_ * [int]$_ } | Measure-Object -Sum).Sum
                } | Select-Object -First 1
                $pixelEvidence.opaque_exterior = [ordered]@{
                    ring_sample_count = $ringSamples.Count
                    samples = $ringSamples.ToArray()
                    closest_sample = $closestSample
                    receiver_background = [ordered]@{
                        sample = @($receiverPoint.X, $receiverPoint.Y)
                        window_from_point = $receiverOwner.hwnd
                        window_from_point_class = $receiverOwner.class
                        window_from_point_pid = $receiverOwner.pid
                        rgb = $receiverRgb
                    }
                    composition = $hostProbe.composition
                    width_pixels = $hostProbe.border_width_pixels
                }

                $centerOwner = Get-PointOwner -X $center[0] -Y $center[1]
                $pixelEvidence.center_hit_target_process = ($centerOwner.pid -eq [uint32]$FixturePid)
            } else {
                $pixelEvidence.opaque_exterior = 'not-sampled; fixture process is no longer running'
            }

            $hostState = 'running; exact sibling host owns reported host and overlay HWNDs'
            if ($WaitForHostExit) {
                $stopTimer = [Diagnostics.Stopwatch]::StartNew()
                while (-not $hostProcess.HasExited -and $stopTimer.Elapsed.TotalSeconds -lt $WaitTimeoutSeconds) {
                    Start-Sleep -Milliseconds 200
                    $hostProcess.Refresh()
                }

                if (-not $hostProcess.HasExited) {
                    throw "Host PID $hostPid is still running. Stop it with the fixture's visible Stop test host button."
                }
                $hostExitCode = $hostProcess.ExitCode
                Start-Sleep -Milliseconds 250
                $hostWindowExists = [WindowSweaters.Probe.Native]::IsWindow($hostWindow)
                $overlayWindowExists = [WindowSweaters.Probe.Native]::IsWindow($overlayWindow)
                $shutdownEvidence = [ordered]@{
                    host_pid = $hostPid
                    exit_code = $hostExitCode
                    host_hwnd_destroyed = -not $hostWindowExists
                    overlay_hwnd_destroyed = -not $overlayWindowExists
                }
                if ($hostExitCode -ne 0 -or $hostWindowExists -or $overlayWindowExists) {
                    throw "Host shutdown was not clean: exit=$hostExitCode, host_hwnd_alive=$hostWindowExists, overlay_hwnd_alive=$overlayWindowExists."
                }
                if ($fixtureReadyProcess) {
                    $shutdownPixels = [System.Collections.Generic.List[object]]::new()
                    foreach ($sample in $pixelEvidence.opaque_exterior.samples) {
                        $sampleOwner = Get-PointOwner -X ([int]$sample.sample[0]) -Y ([int]$sample.sample[1])
                        $sampleRgb = $null
                        if ($sampleOwner.pid -eq [uint32]$FixturePid -and $sampleOwner.window -eq $fixtureReceiver) {
                            $sampleColor = [WindowSweaters.Probe.Native]::GetScreenPixel(
                                [int]$sample.sample[0],
                                [int]$sample.sample[1])
                            $sampleRgb = Get-Rgb -ColorRef $sampleColor
                        }
                        $shutdownPixels.Add([ordered]@{
                            sample = $sample.sample
                            window_from_point_pid = $sampleOwner.pid
                            rgb = $sampleRgb
                        })
                    }
                    $shutdownEvidence.exterior_ring_after_stop = $shutdownPixels.ToArray()
                }
                $hostState = 'stopped cleanly through fixture-owned host window'
            }
        } else {
            $hostExitEvent = $fixtureRecords |
                Where-Object { $_.type -eq 'host.lifecycle' -and $_.event -in @('stopped', 'failed') } |
                Select-Object -Last 1
            $hostWindowExists = [WindowSweaters.Probe.Native]::IsWindow($hostWindow)
            $overlayWindowExists = [WindowSweaters.Probe.Native]::IsWindow($overlayWindow)
            $hostState = if ($hostExitEvent -and $hostExitEvent.event -eq 'stopped' -and $hostExitEvent.code -eq 0) {
                'stopped cleanly; retained fixture/host evidence only'
            } else {
                'exited; retained probe JSON only'
            }
            $shutdownEvidence = [ordered]@{
                host_pid = $hostPid
                fixture_observed_exit = if ($hostExitEvent) { $hostExitEvent.event } else { 'not-recorded' }
                exit_code = if ($hostExitEvent) { $hostExitEvent.code } else { $null }
                host_hwnd_destroyed = -not $hostWindowExists
                overlay_hwnd_destroyed = -not $overlayWindowExists
            }
            if ($WaitForHostExit -and
                ($hostState -notlike 'stopped cleanly*' -or $hostWindowExists -or $overlayWindowExists)) {
                throw "Saved host lifecycle evidence does not prove clean shutdown for PID $hostPid."
            }
        }
    }

    $fixtureRecords = Read-Ndjson -Path $fixtureRecordsPath
    $summary = [ordered]@{
        schema_version = 1
        observed_utc = [DateTime]::UtcNow.ToString('o')
        configuration = $Configuration
        desktop_actions = 'none; computer-use client owns all UI actions'
        fixture = [ordered]@{
            pid = $FixturePid
            state = $fixtureState
            target_hwnd = $fixtureReady.target_hwnd
            target_class = $fixtureReady.target_class
            target_title = $fixtureReady.target_title
            dpi_awareness = $fixtureReady.dpi_awareness
            target_bounds = if ($fixtureReadyProcess) { Get-RectArray -Window $fixtureTarget } else { $fixtureReady.target_bounds }
            result_file = $fixtureRecordsPath
            message_count = @($fixtureRecords | Where-Object type -eq 'window.message').Count
            counters = [ordered]@{
                center_clicks = Get-MessageCount -Records $fixtureRecords -Role 'target' -Message 'WM_LBUTTONDOWN'
                receiver_clicks = Get-MessageCount -Records $fixtureRecords -Role 'receiver' -Message 'WM_LBUTTONDOWN'
                wheel = @($fixtureRecords | Where-Object { $_.type -eq 'window.message' -and $_.message -eq 'WM_MOUSEWHEEL' }).Count
                key_down = @($fixtureRecords | Where-Object { $_.type -eq 'window.message' -and $_.message -eq 'WM_KEYDOWN' }).Count
                characters = @($fixtureRecords | Where-Object { $_.type -eq 'window.message' -and $_.message -eq 'WM_CHAR' }).Count
                focus_changes = @($fixtureRecords | Where-Object {
                    $_.type -eq 'window.message' -and $_.message -in @('WM_SETFOCUS', 'WM_KILLFOCUS')
                }).Count
                controls = @($fixtureRecords | Where-Object type -eq 'control.command')
            }
        }
        host = [ordered]@{
            state = $hostState
            probe_file = $hostProbePath
            pid = if ($hostProbe) { $hostProbe.host_pid } else { $null }
            overlay_hwnd = if ($hostProbe) { $hostProbe.overlay_hwnd } else { $null }
            composition = if ($hostProbe) { $hostProbe.composition } else { $null }
            shutdown = $shutdownEvidence
        }
        visible_pixels = $pixelEvidence
        taskbar_alt_tab = 'not measured; inspect the computer-use window list or Alt+Tab separately'
        fixture_lifecycle = [ordered]@{
            target_destroyed = @($fixtureRecords | Where-Object {
                $_.type -eq 'window.message' -and $_.role -eq 'target' -and $_.message -eq 'WM_DESTROY'
            }).Count -gt 0
            receiver_destroyed = @($fixtureRecords | Where-Object {
                $_.type -eq 'window.message' -and $_.role -eq 'receiver' -and $_.message -eq 'WM_DESTROY'
            }).Count -gt 0
        }
        failure = $failure
    }
} catch {
    $failure = $_.Exception.Message
    if (-not $fixtureRecords) {
        try { $fixtureRecords = Read-Ndjson -Path $fixtureRecordsPath } catch { $fixtureRecords = @() }
    }
    $summary = [ordered]@{
        schema_version = 1
        observed_utc = [DateTime]::UtcNow.ToString('o')
        configuration = $Configuration
        fixture_pid = $FixturePid
        fixture_result_file = $fixtureRecordsPath
        desktop_actions = 'none; computer-use client owns all UI actions'
        host_state = if ($hostProbe) { 'probe-read' } else { 'unknown' }
        visible_pixels = $pixelEvidence
        failure = $failure
    }
} finally {
    $summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $summaryPath -Encoding utf8
    Write-Output "EVIDENCE: $summaryPath"
    Write-Output ($summary | ConvertTo-Json -Depth 10)
}

if ($failure) {
    throw $failure
}
