# Manual reset using the restored encoder and original timing.
param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Capture,
    [Parameter(Mandatory)][ValidateSet(36,40,44,48,149,153,157,161,165)][int]$Channel
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Capture) -or $Capture -match '["\r\n]') {
    throw 'Choose a valid local display identifier.'
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$logDir = Join-Path $repo '.local/stream'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$admin = ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Start-Process powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Capture `"$Capture`" -Channel $Channel" | Out-Null
    exit
}
$resetMutex = [Threading.Mutex]::new($false, 'Local\ZiiUScreen.Reset')
$ownsReset = $false
try {
    $ownsReset = $resetMutex.WaitOne(0)
    if (-not $ownsReset) { throw 'A reset session is already running.' }
    "administrator=$admin; reset_pid=$PID; started=$([DateTime]::UtcNow.ToString('o'))" | Set-Content -LiteralPath "$logDir/reset.log"
    & (Join-Path $repo 'build/ziiuctl/Release/ziiuctl.exe') stream-stop 2>&1 | Add-Content -LiteralPath "$logDir/reset.log"
    $launcherMutex = [Threading.Mutex]::new($false, 'Local\ZiiUScreen.Launcher')
    $ownsLauncher = $false
    try {
        try { $ownsLauncher = $launcherMutex.WaitOne(15000) }
        catch [Threading.AbandonedMutexException] { $ownsLauncher = $true }
        if (-not $ownsLauncher) { throw 'Previous stream has not completed cleanup; no new stream started.' }
    } finally {
        if ($ownsLauncher) { $launcherMutex.ReleaseMutex() }
        $launcherMutex.Dispose()
    }
    'Previous launcher released; starting the restored encoder continuously.' | Add-Content -LiteralPath "$logDir/reset.log"
    # Release reset ownership before the blocking stream runner, so the button
    # can stop and replace this session again later.
    $resetMutex.ReleaseMutex()
    $ownsReset = $false
    & (Join-Path $PSScriptRoot 'run.ps1') -UseSavedEncoder -Seconds 0 -Capture $Capture -Partitions 0 -Channel $Channel -Rate 54
} catch {
    "Reset failed: $($_.Exception.Message)" | Add-Content -LiteralPath "$logDir/reset.log"
    throw
} finally {
    if ($ownsReset) { $resetMutex.ReleaseMutex() }
    $resetMutex.Dispose()
}
