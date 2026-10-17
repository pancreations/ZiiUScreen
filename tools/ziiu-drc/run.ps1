# Normal desktop stream: runs until explicitly stopped. Use -Seconds for a timed
# diagnostic. Start from an elevated PowerShell; -Stop does not need the driver.
param(
    [switch]$Stop,
    [switch]$UseSavedEncoder = $true,
    [switch]$RumbleTest,
    [switch]$PreFeatures,
    # Loopback only hears apps routed to the selected Windows output. Capture the
    # default output normally; 'display' is opt-in for apps explicitly routed to
    # the streamed display's HDMI audio. A name substring or 'off' also works.
    [string]$Audio = 'default',
    # GamePad IR sensor bar on from the start (toggle anytime: TV button or ir.ps1).
    [switch]$SensorBar,
    # ziiu-input options: map buttons by printed letter instead of position; flip stick Y.
    [switch]$InputLabels,
    [switch]$InvertY,
    # Leave the stream processes at normal priority (the old behaviour).
    [switch]$NormalPriority,
    # Diagnostic: hostapd steps through its TX modes, 10 s each, ending in mode 3 (AC
    # queue, no TX status). Also on while .local/stream/tx-rotate.flag exists.
    [switch]$TxRotate,
    [ValidateRange(0,86400)][int]$Seconds = 0,
    [string]$Capture = '',
    [ValidateSet('slow','medium','fast','faster','veryfast')][string]$Preset = 'fast',
    [ValidateRange(-1,16684)][int]$DelayUs = -1,
    # Opt-in late capture, preserving the explicit stamp-to-send delay.
    [ValidateRange(0,16000)][int]$CaptureOffsetUs = 0,
    [ValidateRange(0,1)][int]$Partitions = 0,
    [ValidateSet(36,40,44,48,149,153,157,161,165)][int]$Channel = 161,
    [ValidateSet(6,9,12,18,24,36,48,54)][int]$Rate = 54
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$ctl = Join-Path $repo 'build/ziiuctl/Release/ziiuctl.exe'
if ($Stop) {
    & $ctl stream-stop
    exit $LASTEXITCODE
}
if ([string]::IsNullOrWhiteSpace($Capture)) {
    throw 'Choose your intended display with -Capture before starting a stream.'
}
if (-not $PSBoundParameters.ContainsKey('Channel')) {
    throw 'Choose a supported and locally permitted 5 GHz channel with -Channel.'
}
# The source in the tree is the restored video baseline (beafd64) plus GamePad
# features, so the normal build is used with the baseline's settings. -PreFeatures
# runs the preserved, unmodified baseline binary instead (no rumble/input features).
$videoExe = Join-Path $repo '.local/build/ziiu-drc/ziiu-drc.exe'
if ($PreFeatures) { $videoExe = Join-Path $repo '.local/build/ziiu-drc/ziiu-drc.pre-optimization.exe' }
# Default timing: the baseline's next-tick release (-DelayUs -1) with the `fast`
# preset. Releasing 4 ms after the stamp (-DelayUs 4000) put the GamePad in a
# continuous resync storm on this source, also with nominal-tick stamps (2026-10-15).
# -UseSavedEncoder is kept for Reset connection.cmd and no longer changes settings.
if (-not (Test-Path -LiteralPath $videoExe)) { throw "Encoder executable is missing: $videoExe" }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Start this script from an administrator PowerShell to access the dedicated Wi-Fi driver.'
}
$mutex = [Threading.Mutex]::new($false, 'Local\ZiiUScreen.Launcher')
$ownsMutex = $false
$video = $null
$radio = $null
$logDir = Join-Path $repo '.local/stream'
$names = @('ZIIU_CAPTURE','ZIIU_DRC_DELAY','ZIIU_DRC_DELAY_US','ZIIU_PARTITIONS','ZIIU_DATA_RATE',
           'ZIIU_REPLAY','ZIIU_CAPTURE_DUMP','ZIIU_CAPTURE_DUMP_FRAMES','ZIIU_CRF','ZIIU_VBV_KBPS','ZIIU_VBV_BUF_KBIT','ZIIU_MV_RANGE','ZIIU_INTRA_REFRESH',
           'ZIIU_RUMBLE','ZIIU_RUMBLE_TEST','ZIIU_INPUT_FWD','ZIIU_INPUT_EXTERNAL','ZIIU_AUDIO','ZIIU_SENSOR_BAR',
           'ZIIU_CAPTURE_OFFSET_US','ZIIU_TEST_PORT_OFFSET','ZIIU_IDR_BACKOFF','ZIIU_IDR_GUARD_MAX','ZIIU_STAMP_AFTER_GRAB','ZIIU_TX_ROTATE')
$inputExe = Join-Path $repo 'build/ziiu-input/Release/ziiu-input.exe'
$inputProc = $null
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $ownsMutex = $mutex.WaitOne(0)
    if (-not $ownsMutex) { throw 'A stream launcher is already running. Stop it before starting another.' }
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    $env:ZIIU_CAPTURE = $Capture
    $env:ZIIU_DRC_DELAY = '1'
    $env:ZIIU_PARTITIONS = "$Partitions"
    $env:ZIIU_DATA_RATE = "$Rate"
    if ($TxRotate -or (Test-Path -LiteralPath (Join-Path $logDir 'tx-rotate.flag'))) { $env:ZIIU_TX_ROTATE = '1' }
    if ($DelayUs -ge 0) { $env:ZIIU_DRC_DELAY_US = "$DelayUs" }
    if ($CaptureOffsetUs -gt 0) {
        if ($DelayUs -le $CaptureOffsetUs) { throw 'CaptureOffsetUs requires a larger explicit DelayUs.' }
        $env:ZIIU_CAPTURE_OFFSET_US = "$CaptureOffsetUs"
    }
    $env:ZIIU_RUMBLE = '1'
    if ($RumbleTest) { $env:ZIIU_RUMBLE_TEST = '1' }
    # Selecting the display endpoint does not route game audio to it. Use this only
    # when the game is already playing to that output in Windows' volume mixer.
    if ($Audio -eq 'display') {
        $Audio = 'default'
        $mon = Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "DISPLAY\$Capture\*" } | Select-Object -First 1
        if ($mon) {
            $edid = (Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Enum\$($mon.InstanceId)\Device Parameters" -ErrorAction SilentlyContinue).EDID
            for ($d = 54; $edid -and $d -lt 126; $d += 18) {
                if ($edid[$d] -eq 0 -and $edid[$d + 3] -eq 0xFC) { $Audio = (-join ($edid[($d + 5)..($d + 17)] | ForEach-Object { [char]$_ })).Trim() }
            }
        }
        if ($Audio -eq 'default') { Write-Warning "No EDID name for display $Capture; GamePad audio follows the Windows default output." }
    }
    $env:ZIIU_AUDIO = $Audio
    if ($SensorBar) { $env:ZIIU_SENSOR_BAR = '1' }
    # Controller and touch: ziiu-input takes the GamePad's input port, presents a
    # virtual Xbox 360 controller (ViGEmBus), injects touch on the streamed display
    # and sends game force feedback back as rumble. Optional: the stream runs without it.
    if (Test-Path -LiteralPath $inputExe) {
        $env:ZIIU_INPUT_EXTERNAL = '1'
        $calib = Join-Path $repo '.local/input-calibration.txt'
        $inputArgs = @('--display',$Capture,'--calib',"`"$calib`"")
        if ($InputLabels) { $inputArgs += '--labels' }
        if ($InvertY) { $inputArgs += '--invert-y' }
        $inputProc = Start-Process -FilePath $inputExe -ArgumentList $inputArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logDir/input.log" -RedirectStandardError "$logDir/input.err"
    } else {
        Write-Warning "Controller disabled: $inputExe not built (see tools/ziiu-input)."
    }
    $videoSeconds = if ($Seconds) { $Seconds + 15 } else { 0 }
    # The preserved binary maps zero to a 60-second test. A long duration lets
    # the launcher's explicit stop own shutdown without modifying that binary.
    if ($UseSavedEncoder -and -not $Seconds) { $videoSeconds = [int]::MaxValue }
    $video = Start-Process -FilePath $videoExe -ArgumentList $videoSeconds,$Preset -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logDir/encoder.log" -RedirectStandardError "$logDir/encoder.err"
    $radio = Start-Process -FilePath $ctl -ArgumentList 'ap-hostapd',$Channel,$Seconds -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logDir/radio.log" -RedirectStandardError "$logDir/radio.err"
    "running; duration=$Seconds (0=continuous); delay_us=$DelayUs; preset=$Preset; saved_encoder=$UseSavedEncoder" | Set-Content -LiteralPath "$logDir/status.txt"
    Write-Output "Streaming. Stop with: powershell -File tools/ziiu-drc/run.ps1 -Stop"
    # High priority for the stream: with a game using the CPU, the bridge's 1 ms TX poll
    # fell behind (driver submit->free 12-16 ms vs 2-3 ms idle, 2026-10-16) and every
    # frame reached the GamePad late. hostapd is started by ziiuctl, so it is looked
    # up by name once it exists. -NormalPriority keeps the old behaviour.
    $boosted = $NormalPriority
    if (-not $NormalPriority) {
        foreach ($p in @($video, $inputProc)) { if ($p) { try { $p.PriorityClass = 'High' } catch {} } }
    }
    # Windows 11 ignores timeBeginPeriod(1) for hidden/background processes, so the
    # bridge's 1 ms TX poll became the default 15.6 ms tick whenever window focus moved:
    # driver submit->free pinned at 15.56-15.60 ms in every TX mode, every frame late,
    # GamePad resync storm until power-cycled (2026-10-16). Opt every stream process out
    # of timer-resolution and EcoQoS throttling (PROCESS_POWER_THROTTLING_STATE,
    # ControlMask IGNORE_TIMER_RESOLUTION | EXECUTION_SPEED, StateMask 0).
    Add-Type -Namespace ZiiU -Name Throttle -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
static extern bool SetProcessInformation(System.IntPtr process, int infoClass, uint[] info, uint size);
public static bool Exempt(System.IntPtr process) {
    // Version 1, ControlMask 0x4 | 0x1, StateMask 0: never throttle timers or speed.
    return SetProcessInformation(process, 4, new uint[] { 1, 5, 0 }, 12);
}
'@
    $exempt = { param($p) try { [ZiiU.Throttle]::Exempt($p.Handle) } catch { $false } }
    $exempted = @{}
    foreach ($p in @($video, $inputProc, $radio)) { if ($p) { $exempted[$p.Id] = & $exempt $p } }
    while (-not $radio.HasExited) {
        if (-not $boosted) {
            $hp = Get-Process -Name hostapd -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$repo*" }
            if ($hp) {
                foreach ($p in @($hp) + @($radio)) { try { $p.PriorityClass = 'High' } catch {} }
                "priority: high for encoder, input, radio, hostapd" | Add-Content -LiteralPath "$logDir/status.txt"
                $boosted = $true
            }
        }
        foreach ($p in @(Get-Process -Name hostapd -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$repo*" })) {
            if (-not $exempted.ContainsKey($p.Id)) {
                $exempted[$p.Id] = & $exempt $p
                "timer throttling exemption: hostapd $($exempted[$p.Id]); encoder/input/radio $(@($exempted.Values) -join ',')" |
                    Add-Content -LiteralPath "$logDir/status.txt"
            }
        }
        if ($video.HasExited) { throw 'Video encoder exited; stopping the radio. See .local/stream/encoder.err.' }
        Start-Sleep -Milliseconds 200
        $radio.Refresh()
        $video.Refresh()
    }
    if ($radio.ExitCode -ne 0) { throw "Radio exited with code $($radio.ExitCode). See .local/stream/radio.err." }
} finally {
    if ($radio -and -not $radio.HasExited) {
        & $ctl stream-stop *> "$logDir/stop.log"
        if (-not $radio.WaitForExit(10000)) { Write-Warning 'Radio cleanup is still running; inspect the radio log.' }
    }
    if ($video -and -not $video.HasExited) { Stop-Process -Id $video.Id }
    if ($inputProc -and -not $inputProc.HasExited) { Stop-Process -Id $inputProc.Id }
    if ($ownsMutex) {
        'stopped' | Add-Content -LiteralPath "$logDir/status.txt"
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
