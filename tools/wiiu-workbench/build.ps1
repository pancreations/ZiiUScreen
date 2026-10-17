# Windows only, portable toolchain. No driver or system configuration changes.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\..\.."
$bash = Join-Path $repo '.local\devkitpro\msys64\usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw 'See docs/SETUP.md for the portable devkitPro toolchain.' }
$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
Push-Location $PSScriptRoot
try {
    & $bash -lc 'source /etc/profile.d/devkit-env.sh && bash build.sh'
    if ($LASTEXITCODE -ne 0) { throw "make failed ($LASTEXITCODE)" }
} finally { Pop-Location }
