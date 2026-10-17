# Builds ziiu-probe.wuhb with the portable devkitPro toolchain in .local\devkitpro
# (see docs/SETUP.md "Wii U homebrew toolchain"). Nothing is installed system-wide.
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\..\.."
$bash = Join-Path $repo '.local\devkitpro\msys64\usr\bin\bash.exe'
if (-not (Test-Path $bash)) {
    throw "Portable devkitPro not found at $bash. See docs/SETUP.md."
}
$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
Push-Location $PSScriptRoot
try {
    & $bash -lc 'source /etc/profile.d/devkit-env.sh && make'
    if ($LASTEXITCODE -ne 0) { throw "make failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
