# assemble-pack.ps1 - build the distributable pack layout from BUILD OUTPUTS
# (no game install needed; used by CI and usable locally after build.ps1).
#
#   pack/
#     game/dwmapi.dll
#     game/ue4ss/UE4SS.dll, UE4SS-settings.ini, Mods/ (incl. RoNTacticalRadio)
#     teamspeak/windows/ron_tactical_radio.dll   (if built)
#     teamspeak/linux/ron_tactical_radio_linux_amd64.so (if present)
#     INSTALL-LINUX.md, INSTALL-WINDOWS.txt
#Requires -Version 5.1
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'pack')
)
$ErrorActionPreference = 'Stop'
$build = Join-Path $Root 'build'

if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }

# --- Game side (from superbuild outputs + RE-UE4SS assets) ---
$outRoot = Join-Path $build 'MyMods\Output'
$modDll   = Get-ChildItem -Recurse $outRoot -Filter 'RoNTacticalRadio.dll' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'Shipping' } | Select-Object -First 1
$ue4ssDll = Get-ChildItem -Recurse $outRoot -Filter 'UE4SS.dll' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'Shipping' } | Select-Object -First 1
$proxyDll = Get-ChildItem -Recurse $outRoot -Filter 'dwmapi.dll' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'Shipping' } | Select-Object -First 1
$assets   = Join-Path $build 'MyMods\RE-UE4SS\assets'

if ($modDll -and $ue4ssDll) {
    $g = Join-Path $OutDir 'game'
    New-Item -ItemType Directory -Force -Path (Join-Path $g 'ue4ss') | Out-Null
    Copy-Item $ue4ssDll.FullName (Join-Path $g 'ue4ss\UE4SS.dll')
    if ($proxyDll) { Copy-Item $proxyDll.FullName (Join-Path $g 'dwmapi.dll') }
    Copy-Item (Join-Path $assets 'UE4SS-settings.ini') (Join-Path $g 'ue4ss')
    Copy-Item -Recurse (Join-Path $assets 'Mods') (Join-Path $g 'ue4ss\Mods')
    $modDir = Join-Path $g 'ue4ss\Mods\RoNTacticalRadio\dlls'
    New-Item -ItemType Directory -Force -Path $modDir | Out-Null
    Copy-Item $modDll.FullName (Join-Path $modDir 'main.dll')
    $modsTxt = Join-Path $g 'ue4ss\Mods\mods.txt'
    $existing = if (Test-Path $modsTxt) { @(Get-Content $modsTxt) } else { @() }
    @('RoNTacticalRadio : 1') + $existing | Set-Content $modsTxt
    Write-Host "game/ assembled" -ForegroundColor Green
} else {
    Write-Warning "Game mod / UE4SS outputs not found under $outRoot - game/ skipped."
}

# --- TeamSpeak plugins ---
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'teamspeak\windows'), (Join-Path $OutDir 'teamspeak\linux') | Out-Null
$winDll = Join-Path $build 'ts3-plugin\Release\ron_tactical_radio.dll'
if (Test-Path $winDll) { Copy-Item $winDll (Join-Path $OutDir 'teamspeak\windows') }
$linSo = Join-Path $Root 'ts3-plugin\dist\ron_tactical_radio_linux_amd64.so'
if (Test-Path $linSo) { Copy-Item $linSo (Join-Path $OutDir 'teamspeak\linux') }

# --- Docs ---
Copy-Item (Join-Path $Root 'docs\INSTALL-LINUX.md') $OutDir
@"
Windows: copy the game/ folder contents into
<game>\ReadyOrNot\Binaries\Win64\, and teamspeak\windows\ron_tactical_radio.dll
into %APPDATA%\TS3Client\plugins\. Enable in TS3 Options > Addons, set capture
to Voice Activity Detection. Linux: see INSTALL-LINUX.md.
"@ | Set-Content (Join-Path $OutDir 'INSTALL-WINDOWS.txt')

Write-Host "Pack assembled at $OutDir" -ForegroundColor Green
