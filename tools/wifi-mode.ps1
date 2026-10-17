<#
.SYNOPSIS
    Switch the MediaTek MT7925 Wi-Fi card between normal Windows Wi-Fi and GamePad mode.

.DESCRIPTION
    Status   Show which driver the card uses, and run the Bluetooth/Ethernet health check.
    Normal   Bind MediaTek's own driver (regular Wi-Fi). Uses the copy in the Windows
             driver store, or the backup in .local\driver-backup if Windows lost it.
    GamePad  Bind the ZiiUScreen driver (card becomes the GamePad radio).
    Backup   Export MediaTek's driver package to .local\driver-backup.

    Only the single PCI device VEN_14C3&DEV_7925 is ever touched. Bluetooth (a separate
    USB device) and Ethernet are never modified; they are only checked afterwards.
    Normal/GamePad/Backup need admin and will self-elevate (UAC prompt).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\wifi-mode.ps1 Status
    powershell -ExecutionPolicy Bypass -File tools\wifi-mode.ps1 Normal
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('Status', 'Normal', 'GamePad', 'Backup')]
    [string]$Mode = 'Status',

    # Internal: used when the script re-launches itself elevated.
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'

$RepoRoot      = Split-Path -Parent $PSScriptRoot
$BackupRoot    = Join-Path $RepoRoot '.local\driver-backup'
$WifiIdPrefix  = 'PCI\VEN_14C3&DEV_7925'
$VendorInfName = 'mtkwecx.inf'
$OurInfName    = 'ziiu_wifi.inf'
$OurProvider   = 'ZiiUScreen'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-WifiDevice {
    $devs = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "$WifiIdPrefix*" })
    if ($devs.Count -ne 1) {
        throw "Expected exactly one MT7925 Wi-Fi device ($WifiIdPrefix), found $($devs.Count). Not touching anything."
    }
    $devs[0]
}

function Get-DevProp($dev, $key) {
    (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName $key -ErrorAction SilentlyContinue).Data
}

# Most specific hardware ID without the REV suffix, e.g. PCI\VEN_14C3&DEV_7925&SUBSYS_XXXXXXXX
function Get-InstallHardwareId($dev) {
    $ids = @(Get-DevProp $dev 'DEVPKEY_Device_HardwareIds')
    $id = $ids | Where-Object { $_ -match 'SUBSYS_[0-9A-F]{8}$' } | Select-Object -First 1
    if (-not $id) { throw "Couldn't find a SUBSYS hardware ID for $($dev.InstanceId)." }
    $id
}

function Show-Status {
    $dev = Get-WifiDevice
    $provider = Get-DevProp $dev 'DEVPKEY_Device_DriverProvider'
    $mode = if ($provider -eq $OurProvider) { 'GamePad' } else { 'Normal' }
    [pscustomobject]@{
        Mode          = $mode
        Device        = $dev.FriendlyName
        DeviceStatus  = $dev.Status
        InstanceId    = $dev.InstanceId
        DriverInf     = Get-DevProp $dev 'DEVPKEY_Device_DriverInfPath'
        DriverProvider= $provider
        DriverVersion = Get-DevProp $dev 'DEVPKEY_Device_DriverVersion'
    } | Format-List
}

function Test-Health {
    $ok = $true
    $bt = @(Get-PnpDevice -PresentOnly -Class Bluetooth | Where-Object { $_.InstanceId -like 'USB\VID*' })
    if ($bt.Count -eq 0) {
        Write-Warning 'Bluetooth: no USB Bluetooth adapter found.'; $ok = $false
    } else {
        foreach ($b in $bt) {
            "Bluetooth: $($b.Status)  $($b.FriendlyName)"
            if ($b.Status -ne 'OK') { $ok = $false }
        }
    }
    $net = Test-NetConnection 1.1.1.1 -Port 443 -InformationLevel Quiet -WarningAction SilentlyContinue
    "Internet (Ethernet): $(if ($net) { 'OK' } else { 'FAILED' })"
    if (-not $net) { $ok = $false }
    if ($ok) { 'Health check: PASS' }
    else { Write-Warning 'Health check: FAIL. If this follows a driver switch, run: wifi-mode.ps1 Normal' }
}

# Driver-store packages whose original INF has the given file name, newest first.
function Find-StorePackage($infName) {
    Get-WindowsDriver -Online -All |
        Where-Object { (Split-Path -Leaf $_.OriginalFileName) -ieq $infName } |
        Sort-Object { [version]$_.Version } -Descending
}

function Install-ForDevice($dev, [string]$infPath) {
    if (-not ('ZiiU.NewDev' -as [type])) {
        Add-Type -Namespace ZiiU -Name NewDev -MemberDefinition @'
[DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool UpdateDriverForPlugAndPlayDevicesW(
    IntPtr hwndParent, string HardwareId, string FullInfPath, uint InstallFlags, out bool bRebootRequired);
'@
    }
    $hwid = Get-InstallHardwareId $dev
    $INSTALLFLAG_FORCE = 0x1; $INSTALLFLAG_NONINTERACTIVE = 0x4
    "Installing $infPath for $hwid ..."
    $reboot = $false
    $ok = [ZiiU.NewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $hwid, $infPath,
        $INSTALLFLAG_FORCE -bor $INSTALLFLAG_NONINTERACTIVE, [ref]$reboot)
    if (-not $ok) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw ("UpdateDriverForPlugAndPlayDevices failed: 0x{0:X8}" -f $err)
    }
    if ($reboot) { Write-Warning 'Windows says a reboot is required to finish.' }
}

function Set-NormalMode {
    $dev = Get-WifiDevice
    $pkg = @(Find-StorePackage $VendorInfName)[0]
    if (-not $pkg) {
        $backup = Get-ChildItem $BackupRoot -Recurse -Filter $VendorInfName -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $backup) {
            throw ("MediaTek driver not in the driver store and no backup in $BackupRoot. " +
                   'Get it from Windows Update (Settings > Windows Update > Advanced options > Optional updates) ' +
                   'or the motherboard/laptop vendor support page.')
        }
        "Restoring MediaTek driver from backup: $($backup.FullName)"
        pnputil /add-driver $backup.FullName | Out-Host
        $pkg = @(Find-StorePackage $VendorInfName)[0]
    }
    Install-ForDevice $dev (Join-Path $env:windir "INF\$($pkg.Driver)")
}

function Test-TestSigning {
    (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control).SystemStartOptions -match 'TESTSIGNING'
}

function Set-GamePadMode {
    if (-not (Test-TestSigning)) {
        throw 'Test-signing is off, so Windows would refuse to load the ZiiUScreen driver. See docs/SETUP.md step 1.'
    }
    $dev = Get-WifiDevice

    # Prefer the freshest build output (newest package folder); fall back to the driver store.
    $built = Get-ChildItem (Join-Path $RepoRoot 'build\driver\x64') -Recurse -Filter $OurInfName -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'ziiu_wifi' } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($built) {
        $cat = Join-Path $built.DirectoryName 'ziiu_wifi.cat'
        $signer = (Get-AuthenticodeSignature $cat).SignerCertificate
        if (-not $signer -or -not (Test-Path "Cert:\LocalMachine\TrustedPublisher\$($signer.Thumbprint)")) {
            throw 'The driver test certificate is not trusted on this PC yet. Run tools\trust-test-cert.ps1 (see docs/SETUP.md).'
        }
        $inf = $built.FullName
    } else {
        $pkg = @(Find-StorePackage $OurInfName)[0]
        if (-not $pkg) { throw "ZiiUScreen driver isn't built yet. Run driver\build.ps1." }
        $inf = Join-Path $env:windir "INF\$($pkg.Driver)"
    }

    # Make sure a way back exists before leaving normal mode.
    if (-not (Get-ChildItem $BackupRoot -Recurse -Filter $VendorInfName -ErrorAction SilentlyContinue)) { Backup-VendorDriver }
    Install-ForDevice $dev $inf
}

function Backup-VendorDriver {
    $pkg = @(Find-StorePackage $VendorInfName)[0]
    if (-not $pkg) { throw 'MediaTek driver package not found in the driver store.' }
    $dst = Join-Path $BackupRoot "mediatek-mt7925-$($pkg.Version)"
    New-Item -ItemType Directory -Force $dst | Out-Null
    pnputil /export-driver $pkg.Driver $dst | Out-Host
    "Backed up $($pkg.Driver) ($($pkg.Version)) to $dst"
}

# --- entry point ---
if ($Mode -ne 'Status' -and -not (Test-Admin)) {
    $log = Join-Path $env:TEMP "ziiu-wifi-mode-$PID.log"
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Mode $Mode -LogPath `"$log`""
    $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
    if (Test-Path $log) { Get-Content $log; Remove-Item $log }
    exit $p.ExitCode
}

if ($LogPath) { Start-Transcript -Path $LogPath | Out-Null }
$code = 0
try {
    switch ($Mode) {
        'Status'  { Show-Status }
        'Normal'  { Set-NormalMode;  Show-Status }
        'GamePad' { Set-GamePadMode; Show-Status }
        'Backup'  { Backup-VendorDriver }
    }
    Test-Health
} catch {
    Write-Error $_ -ErrorAction Continue
    $code = 1
} finally {
    if ($LogPath) { Stop-Transcript | Out-Null }
}
exit $code
