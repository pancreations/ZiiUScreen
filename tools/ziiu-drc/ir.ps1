# GamePad IR sensor bar (for a Wii Remote, e.g. in Dolphin) on a running stream.
# Same as a short press of the GamePad's TV button.
#   powershell -File tools/ziiu-drc/ir.ps1 on|off|toggle
param([ValidateSet('on','off','toggle')][string]$State = 'toggle')
$code = @{ on = 'ZIR1'; off = 'ZIR0'; toggle = 'ZIRT' }[$State]
$bytes = [Text.Encoding]::ASCII.GetBytes($code)
$udp = [Net.Sockets.UdpClient]::new()
try {
    [void]$udp.Send($bytes, $bytes.Length, '127.0.0.1', 50200)
    Write-Output "sensor bar: $State requested (see .local/stream/encoder.log)"
} finally { $udp.Close() }
