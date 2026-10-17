<#
.SYNOPSIS
    Build ziiu_wifi.sys. Output package: build\driver\x64\<Configuration>\ziiu_wifi\
.NOTES
    Uses the 64-bit MSBuild: the WDK's INF verification task only ships an x64 DLL and
    fails to load from the 32-bit MSBuild.
#>
param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "64-bit MSBuild not found under $vs" }

& $msbuild (Join-Path $PSScriptRoot 'ziiu_wifi.sln') /p:Configuration=$Configuration /p:Platform=x64 /m /nologo /v:minimal
exit $LASTEXITCODE
