# Send a rumble command to a running ziiu-drc (started with ZIIU_RUMBLE=1, control
# port 127.0.0.1:50200). Level 0..255; Duration 0 holds until the next command.
#   powershell -File tools/ziiu-drc/rumble.ps1 -Level 255 -Duration 400
#   powershell -File tools/ziiu-drc/rumble.ps1 -Level 0            # stop
param(
    [ValidateRange(0,255)][int]$Level = 255,
    [ValidateRange(0,65535)][int]$Duration = 300
)
$bytes = [byte[]]@($Level, ($Duration -band 0xff), (($Duration -shr 8) -band 0xff))
$udp = [Net.Sockets.UdpClient]::new()
try {
    [void]$udp.Send($bytes, $bytes.Length, '127.0.0.1', 50200)
    Write-Output "sent level=$Level duration=${Duration}ms"
} finally { $udp.Close() }
