# Open the touch + stick calibration screen on the GamePad (running stream).
# Same as holding the GamePad's TV button for 2 seconds. Result is saved to
# .local/input-calibration.txt and used from then on.
#   powershell -File tools/ziiu-input/calibrate.ps1
$bytes = [Text.Encoding]::ASCII.GetBytes('ZCALIBRATE')
$udp = [Net.Sockets.UdpClient]::new()
try {
    [void]$udp.Send($bytes, $bytes.Length, '127.0.0.1', 50222)
    Write-Output 'Calibration screen requested: follow it on the GamePad.'
} finally { $udp.Close() }
