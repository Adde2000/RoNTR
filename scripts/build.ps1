# build.ps1 - build the TS3 plugin and/or the UE4SS game mod (Release).
#Requires -Version 5.1
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [switch]$GameModOnly,
    [switch]$TsPluginOnly,
    # RE-UE4SS uses UE-style config triplets, not Release/Debug.
    [string]$GameModConfig = 'Game__Shipping__Win64'
)
$ErrorActionPreference = 'Stop'
$build = Join-Path $Root 'build'

if (-not $GameModOnly) {
    Write-Host "== Building TS3 plugin ==" -ForegroundColor Cyan
    $sdk = Join-Path $Root 'third_party\ts3client-pluginsdk'
    if (-not (Test-Path $sdk)) { throw "TS3 SDK missing - run scripts\setup.ps1 first." }
    cmake -S (Join-Path $Root 'ts3-plugin') -B (Join-Path $build 'ts3-plugin') -A x64 `
          -DTS3_PLUGINSDK_DIR="$sdk"
    if ($LASTEXITCODE) { throw "TS3 plugin configure failed" }
    cmake --build (Join-Path $build 'ts3-plugin') --config Release
    if ($LASTEXITCODE) { throw "TS3 plugin build failed" }
    Write-Host "TS3 plugin OK: build\ts3-plugin\Release\ron_tactical_radio.dll" -ForegroundColor Green
}

if (-not $TsPluginOnly) {
    Write-Host "== Building game mod (first RE-UE4SS build takes a while) ==" -ForegroundColor Cyan
    $myMods = Join-Path $build 'MyMods'
    if (-not (Test-Path (Join-Path $myMods 'CMakeLists.txt'))) { throw "Run scripts\setup.ps1 first." }
    cmake -S $myMods -B (Join-Path $myMods 'Output')
    if ($LASTEXITCODE) { throw "Game mod configure failed" }
    # Build the mod plus the UE4SS runtime + proxy loader so deploy.ps1 can ship
    # an ABI-matched UE4SS (a release download will NOT load mods built from main).
    cmake --build (Join-Path $myMods 'Output') --config $GameModConfig `
          --target RoNTacticalRadio --target UE4SS --target proxy
    if ($LASTEXITCODE) { throw "Game mod build failed" }
    Write-Host "Game mod OK." -ForegroundColor Green
}
