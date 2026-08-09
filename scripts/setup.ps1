# setup.ps1 - one-time: check prerequisites, clone RE-UE4SS + TS3 Plugin SDK,
# generate the CMake superbuild for the game mod.
#Requires -Version 5.1
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent)
)
$ErrorActionPreference = 'Stop'

function Need([string]$cmd, [string]$hint) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        throw "Missing '$cmd'. $hint"
    }
}

Write-Host "== RoN Tactical Radio setup ==" -ForegroundColor Cyan
Need git   "Install Git for Windows: https://git-scm.com"
Need cmake "Install CMake (3.22+), check 'add to PATH': https://cmake.org/download"
Need cargo "Install Rust via https://rustup.rs (RE-UE4SS's patternsleuth dependency is Rust)"
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Write-Warning "MSVC 'cl' not on PATH. That's fine if Visual Studio 2022 (Desktop C++ workload) is installed - CMake will find it."
}

$build  = Join-Path $Root 'build'
$myMods = Join-Path $build 'MyMods'
New-Item -ItemType Directory -Force -Path $myMods | Out-Null

# --- TeamSpeak 3 Client Plugin SDK ---
$sdk = Join-Path $Root 'third_party\ts3client-pluginsdk'
if (-not (Test-Path $sdk)) {
    Write-Host "Cloning TS3 Client Plugin SDK..."
    git clone --depth 1 https://github.com/teamspeak/ts3client-pluginsdk $sdk
} else { Write-Host "TS3 Plugin SDK already present." }

# --- RE-UE4SS ---
$ue4ss = Join-Path $myMods 'RE-UE4SS'
if (-not (Test-Path $ue4ss)) {
    Write-Host "Cloning RE-UE4SS (large; submodules need a GitHub account linked to Epic Games" `
               "for Unreal source access - see docs.ue4ss.com prerequisites)..."
    git clone https://github.com/UE4SS-RE/RE-UE4SS $ue4ss
    git -C $ue4ss submodule update --init --recursive
} else { Write-Host "RE-UE4SS already present." }

# --- Superbuild CMakeLists (RE-UE4SS + our game mod) ---
$gameModDir = (Join-Path $Root 'game-mod') -replace '\\', '/'
@"
cmake_minimum_required(VERSION 3.22)
project(MyMods)
add_subdirectory(RE-UE4SS)
add_subdirectory("$gameModDir" game-mod)
"@ | Set-Content -Encoding UTF8 (Join-Path $myMods 'CMakeLists.txt')

Write-Host "Setup complete. Next: scripts\build.ps1" -ForegroundColor Green
