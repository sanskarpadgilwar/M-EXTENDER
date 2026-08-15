# Builds the TwinScreen UMDF IddCx driver and stages an installable package.
#
# Prerequisites:
#   - Visual Studio 2022 (Desktop C++ workload, MSBuild)
#   - Windows 10/11 SDK
#   - WDK for Windows 10/11 (10.0.22621 or newer)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
#   # outputs: driver\out\pkg\TwinScreen.{dll,inf,cat}

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio not found (vswhere.exe missing). Install VS2022 with the Desktop C++ workload.'
}

$vsInstall = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
if (-not $vsInstall) {
    throw 'No Visual Studio installation with MSBuild found.'
}

$msbuild = Join-Path $vsInstall 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    throw "MSBuild.exe not found under $vsInstall"
}

$proj  = Join-Path $PSScriptRoot 'TwinScreen.vcxproj'
$stage = Join-Path $PSScriptRoot 'out\obj'
$pkg   = Join-Path $PSScriptRoot 'out\pkg'

Write-Host "Building with $msbuild"

& $msbuild $proj /m /t:Build "/p:Configuration=Release" "/p:Platform=x64" "/p:OutDir=$stage\"
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed (exit code $LASTEXITCODE). Is the WDK (10.0.22621+) installed?"
}

New-Item -ItemType Directory -Force -Path $pkg | Out-Null

Copy-Item -Force (Join-Path $stage 'TwinScreen.dll') $pkg
Copy-Item -Force (Join-Path $stage 'TwinScreen.inf')  $pkg
$cat = Get-ChildItem -Path $stage -Filter 'TwinScreen.cat' -Recurse | Select-Object -First 1
if ($cat) {
    Copy-Item -Force $cat.FullName $pkg
}
Copy-Item -Force (Join-Path $PSScriptRoot 'option.txt') $pkg

Write-Host ''
Write-Host 'Build complete. Package ready:'
Write-Host "  $pkg"
Write-Host ''
Write-Host 'Install (as Administrator):'
Write-Host '  bcdedit /set testsigning on   (then reboot)'
Write-Host "  pnputil /add-driver \"$pkg\TwinScreen.inf\" /install"
