# Build the ZiiUScreen desktop app -> build\ziiu-app\ZiiUScreen.exe
#   powershell -ExecutionPolicy Bypass -File tools\ziiu-app\build.ps1
# Uses a portable .NET 9 SDK in .local\dotnet (downloaded once with Microsoft's
# dotnet-install.ps1; nothing is installed system-wide). The app itself runs on the
# installed .NET 9 Desktop Runtime.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dotnet = Join-Path $repo '.local\dotnet\dotnet.exe'
$out = Join-Path $repo 'build\ziiu-app'

if (-not (Test-Path -LiteralPath $dotnet)) {
    $dl = Join-Path $repo '.local\dotnet-install'
    New-Item -ItemType Directory -Force -Path $dl | Out-Null
    Invoke-WebRequest -UseBasicParsing https://dot.net/v1/dotnet-install.ps1 -OutFile "$dl\dotnet-install.ps1"
    & "$dl\dotnet-install.ps1" -Channel 9.0 -InstallDir (Split-Path $dotnet) -NoPath
}
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'

# Exe/tray icon from ZIIUICON.png: an .ico holding PNG-compressed sizes.
New-Item -ItemType Directory -Force -Path $out | Out-Null
$ico = Join-Path $repo 'build\ziiu-app\obj\ZiiUScreen.ico'
New-Item -ItemType Directory -Force -Path (Split-Path $ico) | Out-Null
Add-Type -AssemblyName System.Drawing
$src = [Drawing.Image]::FromFile((Join-Path $repo 'ZIIUICON.png'))
try {
    $images = foreach ($size in 16, 24, 32, 48, 64, 128, 256) {
        $bmp = [Drawing.Bitmap]::new($size, $size)
        $g = [Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.DrawImage($src, 0, 0, $size, $size)
        $g.Dispose()
        $ms = [IO.MemoryStream]::new()
        $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        , @($size, $ms.ToArray())
    }
} finally { $src.Dispose() }
$fs = [IO.File]::Create($ico)
$w = [IO.BinaryWriter]::new($fs)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $s = $img[0]; $data = $img[1]
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$data.Length); $w.Write([uint32]$offset)
    $offset += $data.Length
}
foreach ($img in $images) { $w.Write($img[1]) }
$w.Dispose()

& $dotnet publish (Join-Path $PSScriptRoot 'ZiiuApp.csproj') -c Release -o $out --nologo -v minimal "-p:ZiiuIcon=$ico" "-p:ZiiuRepo=$repo"
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }
# Single-file exe, copied to the repo root for double-clicking (ignored by git).
$exe = Join-Path $repo 'ZiiUScreen.exe'
Copy-Item -LiteralPath (Join-Path $out 'ZiiUScreen.exe') -Destination $exe -Force
Write-Output "Built $exe"
