# Adds only repo-local SDK libraries. The portable devkitPro base must already exist.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$bash = Join-Path $repo '.local\devkitpro\msys64\usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw 'Install the portable toolchain described in docs/SETUP.md first.' }
$dependencies = @(
    @{ Name = 'WiiUPluginSystem-compatible'; Upstream = 'WiiUPluginSystem'; Commit = '2c76941e9358eda6be86363474434c4db1af860e' },
    @{ Name = 'libmappedmemory'; Upstream = 'libmappedmemory'; Commit = '8a2273bfa47d7c207bda81f3e823026c5be72f92' }
)
$env:MSYSTEM = 'MSYS'
$env:CHERE_INVOKING = '1'
foreach ($dependency in $dependencies) {
    $source = Join-Path $repo ('.local\refs\' + $dependency.Name)
    if (-not (Test-Path $source)) {
        & git clone ('https://github.com/wiiu-env/' + $dependency.Upstream + '.git') $source
        if ($LASTEXITCODE -ne 0) { throw 'Dependency download failed.' }
        & git -C $source checkout --detach $dependency.Commit
        if ($LASTEXITCODE -ne 0) { throw 'Could not select pinned SDK revision.' }
    }
    $head = & git -C $source rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $head -ne $dependency.Commit) {
        throw "Existing $source has another revision; expected $($dependency.Commit). Leaving it unchanged."
    }
    Push-Location $source
    try {
        # libmappedmemory's Makefile must run serially.
        if ($dependency.Upstream -eq 'WiiUPluginSystem') {
            & $bash -lc 'source /etc/profile.d/devkit-env.sh && make'
        } else {
            & $bash -lc 'source /etc/profile.d/devkit-env.sh && make && make install'
        }
        if ($LASTEXITCODE -ne 0) { throw "SDK build failed for $($dependency.Name)." }
    } finally { Pop-Location }
}
