# deploy.ps1 - install built DLLs: game mod -> <game>/Binaries/Win64/ue4ss/Mods,
# TS3 plugin -> %APPDATA%/TS3Client/plugins. Optionally installs UE4SS itself.
#Requires -Version 5.1
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    # Repo lives inside the game's Win64 folder by default; override if not.
    [string]$GameWin64 = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
)
$ErrorActionPreference = 'Stop'
$build = Join-Path $Root 'build'

if (-not (Test-Path (Join-Path $GameWin64 'ReadyOrNot-Win64-Shipping.exe'))) {
    Write-Warning "'$GameWin64' doesn't look like the RoN Win64 folder (pass -GameWin64)."
}

# --- UE4SS runtime: deploy the one WE built (ABI must match the mod DLL). ---
# A downloaded UE4SS release will refuse to load C++ mods built from a
# different commit (error 0x7f ERROR_PROC_NOT_FOUND).
$ue4ssDir = Join-Path $GameWin64 'ue4ss'
$outRoot  = Join-Path $build 'MyMods\Output'
$ue4ssDll = Get-ChildItem -Recurse $outRoot -Filter 'UE4SS.dll'  -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'Shipping' } | Select-Object -First 1
$proxyDll = Get-ChildItem -Recurse $outRoot -Filter 'dwmapi.dll' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'Shipping' } | Select-Object -First 1
if (-not $ue4ssDll) { throw "Built UE4SS.dll not found - run scripts\build.ps1 (it builds UE4SS + proxy too)." }

New-Item -ItemType Directory -Force -Path $ue4ssDir | Out-Null
Copy-Item $ue4ssDll.FullName (Join-Path $ue4ssDir 'UE4SS.dll') -Force
if ($proxyDll) { Copy-Item $proxyDll.FullName (Join-Path $GameWin64 'dwmapi.dll') -Force }
Write-Host "UE4SS runtime (our build) -> $ue4ssDir" -ForegroundColor Green

# First-time extras: settings + default mods from the RE-UE4SS checkout.
$assets = Join-Path $build 'MyMods\RE-UE4SS\assets'
if (-not (Test-Path (Join-Path $ue4ssDir 'UE4SS-settings.ini'))) {
    Copy-Item (Join-Path $assets 'UE4SS-settings.ini') $ue4ssDir
}
if (-not (Test-Path (Join-Path $ue4ssDir 'Mods'))) {
    Copy-Item -Recurse (Join-Path $assets 'Mods') (Join-Path $ue4ssDir 'Mods')
}

# Clean up conflicting old-layout installs (UE4SS.dll / mod copies in Win64 root).
if (Test-Path (Join-Path $GameWin64 'UE4SS.dll')) {
    Rename-Item (Join-Path $GameWin64 'UE4SS.dll') 'UE4SS.dll.bak' -Force
    Write-Warning "Old-layout UE4SS.dll in Win64 renamed to .bak (superseded by ue4ss\UE4SS.dll)."
}
$legacyMod = Join-Path $GameWin64 'Mods\RoNTacticalRadio'
if (Test-Path $legacyMod) {
    Remove-Item -Recurse -Force $legacyMod
    Write-Warning "Removed stale copy at Win64\Mods\RoNTacticalRadio (now lives in ue4ss\Mods)."
}

# --- Game mod ---
$modDll = Get-ChildItem -Recurse (Join-Path $build 'MyMods\Output') -Filter 'RoNTacticalRadio.dll' -ErrorAction SilentlyContinue |
          Sort-Object @{e={$_.FullName -match 'Shipping'};Descending=$true},
                      @{e={$_.LastWriteTime};Descending=$true} |
          Select-Object -First 1
if (-not $modDll) { throw "RoNTacticalRadio.dll not built - run scripts\build.ps1 first." }
$modDir = Join-Path $ue4ssDir 'Mods\RoNTacticalRadio\dlls'
New-Item -ItemType Directory -Force -Path $modDir | Out-Null
Copy-Item $modDll.FullName (Join-Path $modDir 'main.dll') -Force
Write-Host "Game mod -> $modDir\main.dll" -ForegroundColor Green

# Enable in mods.txt
$modsTxt = Join-Path $ue4ssDir 'Mods\mods.txt'
if (Test-Path $modsTxt) {
    $content = @(Get-Content $modsTxt)
    if (@($content -match 'RoNTacticalRadio').Count -eq 0) {
        @("RoNTacticalRadio : 1") + $content | Set-Content $modsTxt
        Write-Host "Enabled in mods.txt"
    }
} else {
    Set-Content $modsTxt "RoNTacticalRadio : 1"
}

# --- TS3 plugin ---
$tsDll = Join-Path $build 'ts3-plugin\Release\ron_tactical_radio.dll'
if (Test-Path $tsDll) {
    if (Get-Process ts3client_win64 -ErrorAction SilentlyContinue) {
        Write-Warning "TeamSpeak is running - close it so the plugin DLL can be replaced."
    }
    $tsPlugins = Join-Path $env:APPDATA 'TS3Client\plugins'
    New-Item -ItemType Directory -Force -Path $tsPlugins | Out-Null
    Copy-Item $tsDll $tsPlugins -Force
    Write-Host "TS3 plugin -> $tsPlugins\ron_tactical_radio.dll" -ForegroundColor Green
    Write-Host "Enable it in TS3: Tools > Options > Addons. Set capture to Continuous Transmission."
} else {
    Write-Warning "TS3 plugin not built; skipped."
}

Write-Host "`nSmoke test: start TS3 (plugin enabled), launch RoN, join a co-op lobby." -ForegroundColor Cyan
Write-Host "UE4SS console should log '[RoNTacticalRadio] shared memory ready'."
