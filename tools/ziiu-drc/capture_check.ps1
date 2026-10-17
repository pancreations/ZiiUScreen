# Offline A/B for the desktop path, no radio: scroll text on the dummy display for
# 5 s, then freeze; ziiu-drc frames 120..419 are dumped, the packets it sends are
# decoded with the GamePad's SPS/PPS, and luma/chroma PSNR is printed per frame.
# Any ZIIU_* variable set by the caller (ZIIU_CRF, ZIIU_MV_RANGE, ...) is passed on.
#
#   tools\ziiu-drc\capture_check.ps1 -Python <python with numpy> -Ffmpeg <ffmpeg.exe>
param(
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$Ffmpeg,
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Monitor,
    [string]$OutDir = "$PSScriptRoot\..\..\.local\capture-check"
)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$m = Start-Process -FilePath $Python -ArgumentList @('-I', "$PSScriptRoot\motion_load.py", $Monitor, 12, 5.0) -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1500
$env:ZIIU_CAPTURE = $Monitor
$env:ZIIU_CAPTURE_DUMP = "$OutDir\src.yuv"
$env:ZIIU_CAPTURE_DUMP_FRAMES = '300'
$env:ZIIU_CHECK_SECONDS = '8'
$env:ZIIU_CHECK_DIR = "$OutDir\chk"
$env:ZIIU_DRC_DELAY = '1'
& $Python -I "$PSScriptRoot\check_stream.py" $Ffmpeg 2>&1 |
    Select-String '^(U |V |x264|capture:|vstrm|ffmpeg|  frame|min|FAIL|video|timing|health|frames=|encode failure)' | ForEach-Object { $_.Line }
$checkExit = $LASTEXITCODE
$m | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
if ($checkExit -ne 0) { throw "Stream check failed (exit $checkExit)" }
