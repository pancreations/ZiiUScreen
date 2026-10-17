<#
.SYNOPSIS
    Download the MediaTek MT7925 Wi-Fi firmware into driver\firmware\.

.DESCRIPTION
    Fetches the two files mt76 uses for the MT7925 (ROM patch + WM firmware) and their
    licence from linux-firmware on kernel.org, pinned to a release tag, and checks
    SHA-256 hashes. The firmware is redistributable for use with MediaTek chipsets
    (LICENCE.mediatek). It is not committed to this repo.

    To move to a newer linux-firmware release: change $Tag, run with -UpdateHashes,
    test fw-load, and commit the new hashes.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\fetch-firmware.ps1
#>
param([switch]$UpdateHashes)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Tag  = '20260916'
$Base = 'https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain'
$Files = [ordered]@{
    'mediatek/mt7925/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin' = '8eb46014d2a6b4124472eee7476d995008a6f40b1daffef87eb42f30d98699e1'
    'mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin'      = '23ff53b4bb639b30481e2e06bb1688569ad1ba971b897936db539882abfbd120'
    'LICENSES/LICENCE.mediatek'                          = 'a90d3f66704d85889945fec5525ea77622549da83aced1aac99828383f8f1805'
}

$Dest = Join-Path (Split-Path -Parent $PSScriptRoot) 'driver\firmware'
New-Item -ItemType Directory -Force $Dest | Out-Null

foreach ($path in $Files.Keys) {
    $out = Join-Path $Dest (Split-Path -Leaf $path)
    $url = "$Base/$path`?h=$Tag"
    "Downloading $path ($Tag)"
    # Non-browser user agent: kernel.org answers browser-like agents (PowerShell's
    # default) with an anti-bot HTML page. The hash check below catches that anyway.
    Invoke-WebRequest -UseBasicParsing -UserAgent 'ZiiUScreen-fetch-firmware' -Uri $url -OutFile $out
    $hash = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLowerInvariant()
    if ($UpdateHashes) {
        "  sha256 $hash"
    } elseif ($hash -ne $Files[$path]) {
        Remove-Item $out
        throw "SHA-256 mismatch for $path (got $hash). Not using it."
    } else {
        '  sha256 OK'
    }
}
"linux-firmware $Tag" | Set-Content -Encoding ascii (Join-Path $Dest 'SOURCE.txt')
"Firmware is in $Dest"
