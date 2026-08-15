# build-linux-plugin.ps1 - build the Linux TS3 plugin .so from Windows.
# Runs scripts/build-linux-plugin.sh inside WSL (needs a distro with g++ and
# git: `wsl sudo apt install g++ git`). Output:
#   ts3-plugin/dist/ron_tactical_radio_linux_amd64.so
#Requires -Version 5.1
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [string]$Distro = ''   # default WSL distro when empty
)
$ErrorActionPreference = 'Stop'

if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) {
    throw "WSL is required (install a distro with g++), or run scripts/build-linux-plugin.sh on a Linux box."
}

# D:\Projects\RoNTR -> /mnt/d/Projects/RoNTR
$drive = $Root.Substring(0, 1).ToLower()
$wslRoot = "/mnt/$drive" + ($Root.Substring(2) -replace '\\', '/')

$wslArgs = @()
if ($Distro) { $wslArgs += @('-d', $Distro) }
$wslArgs += @('--', 'bash', '-lc', "cd '$wslRoot' && ./scripts/build-linux-plugin.sh")

Write-Host "== Building Linux TS3 plugin (WSL) ==" -ForegroundColor Cyan
wsl @wslArgs
if ($LASTEXITCODE) { throw "Linux plugin build failed" }
Write-Host "Linux plugin OK: ts3-plugin\dist\ron_tactical_radio_linux_amd64.so" -ForegroundColor Green
