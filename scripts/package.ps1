# package.ps1 - build a shareable friend-pack zip: game files (from your
# deployed, working install) + TS plugins for Windows and Linux + install docs.
#Requires -Version 5.1
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [string]$GameWin64 = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)
)
$ErrorActionPreference = 'Stop'

$stage = Join-Path $env:TEMP 'RoN-TacticalRadio-pack'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }

# --- Game files (exactly what deploy.ps1 installed and you verified) ---
$g = Join-Path $stage 'game'
New-Item -ItemType Directory -Force -Path (Join-Path $g 'ue4ss') | Out-Null
Copy-Item (Join-Path $GameWin64 'dwmapi.dll')              $g
Copy-Item (Join-Path $GameWin64 'ue4ss\UE4SS.dll')         (Join-Path $g 'ue4ss')
Copy-Item (Join-Path $GameWin64 'ue4ss\UE4SS-settings.ini')(Join-Path $g 'ue4ss')
Copy-Item -Recurse (Join-Path $GameWin64 'ue4ss\Mods')     (Join-Path $g 'ue4ss\Mods')
Get-ChildItem -Recurse (Join-Path $g 'ue4ss') -Include '*.log' | Remove-Item -Force

# --- TeamSpeak plugins ---
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'teamspeak\windows'), (Join-Path $stage 'teamspeak\linux') | Out-Null
$winDll = Join-Path $Root 'build\ts3-plugin\Release\ron_tactical_radio.dll'
if (Test-Path $winDll) { Copy-Item $winDll (Join-Path $stage 'teamspeak\windows') }
$linSo = Join-Path $Root 'ts3-plugin\dist\ron_tactical_radio_linux_amd64.so'
if (Test-Path $linSo) { Copy-Item $linSo (Join-Path $stage 'teamspeak\linux') }
else { Write-Warning "Linux .so not found at $linSo (build with scripts/build-linux-plugin.sh or grab it from the repo)." }

# --- Docs + source for local rebuilds ---
Copy-Item (Join-Path $Root 'docs\INSTALL-LINUX.md') $stage
@"
Windows friends: copy the game/ folder contents into
<game>\ReadyOrNot\Binaries\Win64\, and teamspeak\windows\ron_tactical_radio.dll
into %APPDATA%\TS3Client\plugins\. Enable in TS3 Options > Addons, set capture
to Continuous Transmission. Linux friends: see INSTALL-LINUX.md.
"@ | Set-Content (Join-Path $stage 'INSTALL-WINDOWS.txt')

$out = Join-Path $Root 'dist'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$tarball = Join-Path $out 'RoN-TacticalRadio-friend-pack.tar.gz'
if (Test-Path $tarball) { Remove-Item $tarball }
# tar.exe (bsdtar) ships with Windows 10+; -C so paths in the archive are relative.
tar -czf $tarball -C $stage .
if ($LASTEXITCODE) { throw "tar failed" }
Write-Host "Friend pack: $tarball" -ForegroundColor Green
