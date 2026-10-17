<#
.SYNOPSIS
    Trust (or stop trusting) the self-made test certificate that signs ziiu_wifi.sys.

.DESCRIPTION
    The WDK signs development builds with a per-user test certificate
    ("WDKTestCert <user>,..."). For Windows to install the driver package without a
    prompt, that certificate must be in the machine's Trusted Root and Trusted
    Publishers stores. This only affects drivers signed with *this* certificate, whose
    private key exists only in your user profile on this PC.

    Run as administrator.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\trust-test-cert.ps1
    powershell -ExecutionPolicy Bypass -File tools\trust-test-cert.ps1 -Remove
#>
param([switch]$Remove)
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$cat = Get-ChildItem (Join-Path $RepoRoot 'build\driver\x64') -Recurse -Filter ziiu_wifi.cat -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $cat) { throw 'No built driver found. Run driver\build.ps1 first.' }
$cert = (Get-AuthenticodeSignature $cat.FullName).SignerCertificate
if (-not $cert) { throw "$($cat.FullName) is not signed." }
if ($cert.Subject -notlike '*WDKTestCert*') { throw "Unexpected signer '$($cert.Subject)'. Refusing to trust it." }

foreach ($storeName in 'Root', 'TrustedPublisher') {
    $store = New-Object Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
    $store.Open('ReadWrite')
    if ($Remove) { $store.Remove($cert); "Removed from LocalMachine\$storeName" }
    else { $store.Add($cert); "Added to LocalMachine\$storeName" }
    $store.Close()
}
"Certificate: $($cert.Subject)  $($cert.Thumbprint)"
