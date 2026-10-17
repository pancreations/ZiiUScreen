<#
.SYNOPSIS
    Build the ZiiUScreen virtual monitor (UMDF IddCx). Output package:
    build\display\x64\<Configuration>\ziiu_display\ (separate from the Wi-Fi driver's
    build\driver, which tools\wifi-mode.ps1 installs).
#>
param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "64-bit MSBuild not found under $vs" }

& $msbuild (Join-Path $PSScriptRoot 'ziiu_display.sln') /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /v:minimal
exit $LASTEXITCODE
