# Builds ziiu-live-probe.wuhb with the portable toolchain in .local\devkitpro.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\..\.."
$bash = Join-Path $repo '.local\devkitpro\msys64\usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw "Portable devkitPro not found at $bash. See docs/SETUP.md." }
$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
Push-Location $PSScriptRoot
try {
    & $bash -lc 'source /etc/profile.d/devkit-env.sh && make'
    if ($LASTEXITCODE -ne 0) { throw "make failed ($LASTEXITCODE)" }
} finally { Pop-Location }
