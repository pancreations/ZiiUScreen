# Offline check of ziiu-input, no radio or GamePad: synthetic input reports in,
# Windows XInput state out, and an XInput rumble request back on the rumble port.
# Adds a virtual Xbox 360 controller for a few seconds (needs ViGEmBus). Stop the stream first:
# ziiu-input takes the bridge's input port 50022. Touch is not injected (it would tap the desktop).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$exe = Join-Path $repo 'build/ziiu-input/Release/ziiu-input.exe'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class XI {
  [StructLayout(LayoutKind.Sequential)] public struct Pad { public ushort Buttons; public byte LT, RT; public short LX, LY, RX, RY; }
  [StructLayout(LayoutKind.Sequential)] public struct State { public uint Packet; public Pad Gamepad; }
  [StructLayout(LayoutKind.Sequential)] public struct Vib { public ushort L, R; }
  [DllImport("xinput1_4.dll")] public static extern uint XInputGetState(uint i, out State s);
  [DllImport("xinput1_4.dll")] public static extern uint XInputSetState(uint i, ref Vib v);
}
'@
function Report([int]$buttons, [int]$ly) {
    $d = [byte[]]::new(128)
    $d[2] = ($buttons -shr 8) -band 0xff; $d[3] = $buttons -band 0xff
    foreach ($i in 0..3) { $v = if ($i -eq 1) { $ly } else { 2050 }; $d[6 + 2*$i] = $v -band 0xff; $d[7 + 2*$i] = $v -shr 8 }
    $d
}
$log = Join-Path $env:TEMP 'ziiu-input-selftest.log'
$before = @(0..3 | Where-Object { $s = New-Object XI+State; [XI]::XInputGetState($_, [ref]$s) -eq 0 })
$p = Start-Process $exe -PassThru -WindowStyle Hidden -RedirectStandardOutput $log
$tx = [Net.Sockets.UdpClient]::new()
$rx = [Net.Sockets.UdpClient]::new([Net.IPEndPoint]::new([Net.IPAddress]::Loopback, 50200))
$rx.Client.ReceiveTimeout = 1500
$fail = 0
try {
    Start-Sleep -Milliseconds 800
    $idx = @(0..3 | Where-Object { $s = New-Object XI+State; [XI]::XInputGetState($_, [ref]$s) -eq 0 } | Where-Object { $before -notcontains $_ })
    if ($idx.Count -ne 1) { throw "expected one new XInput controller, found $($idx.Count)" }
    $i = [uint32]$idx[0]
    "virtual controller at XInput slot $i"
    $r = Report 0x8000 3200   # GamePad A held, left stick fully at raw maximum
    for ($k = 0; $k -lt 5; $k++) { [void]$tx.Send($r, 128, '127.0.0.1', 50022); Start-Sleep -Milliseconds 10 }
    Start-Sleep -Milliseconds 100
    $s = New-Object XI+State; [void][XI]::XInputGetState($i, [ref]$s)
    "GamePad A + raw LY 3200 -> XInput buttons=0x{0:x4} LY={1}" -f $s.Gamepad.Buttons, $s.Gamepad.LY
    if ($s.Gamepad.Buttons -ne 0x2000) { "FAIL: expected Xbox B (0x2000, position mapping)"; $fail++ }
    if ($s.Gamepad.LY -ne 32767) { "FAIL: expected LY 32767"; $fail++ }
    Start-Sleep -Milliseconds 400   # no reports: controller must release
    [void][XI]::XInputGetState($i, [ref]$s)
    "after 400 ms without reports -> buttons=0x{0:x4} LY={1}" -f $s.Gamepad.Buttons, $s.Gamepad.LY
    if ($s.Gamepad.Buttons -ne 0 -or $s.Gamepad.LY -ne 0) { "FAIL: expected release"; $fail++ }
    $v = New-Object XI+Vib; $v.L = 65535; [void][XI]::XInputSetState($i, [ref]$v)
    $ep = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
    $m = $rx.Receive([ref]$ep)
    "game rumble L=65535 -> rumble port level={0} duration={1} ms" -f $m[0], ($m[1] + 256*$m[2])
    if ($m[0] -ne 255) { "FAIL: expected level 255"; $fail++ }
    $v.L = 0; [void][XI]::XInputSetState($i, [ref]$v)
    do { $m = $rx.Receive([ref]$ep) } while ($m[0] -ne 0)
    "game rumble off -> rumble port level=0"
} finally {
    Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
    $tx.Close(); $rx.Close()
}
Start-Sleep -Milliseconds 500
$after = @(0..3 | Where-Object { $s = New-Object XI+State; [XI]::XInputGetState($_, [ref]$s) -eq 0 })
if ($after.Count -ne $before.Count) { "FAIL: virtual controller still present after exit"; $fail++ } else { "virtual controller removed on exit" }
"--- ziiu-input log ---"; Get-Content $log
if ($fail) { "SELFTEST FAILED ($fail)"; exit 1 } else { "SELFTEST PASSED" }
