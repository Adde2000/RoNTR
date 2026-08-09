# all.ps1 - setup (if needed) + build + deploy in one go.
# UE4SS itself is built from source and deployed by these scripts (ABI-matched
# to the mod), so no separate UE4SS install is needed.
#Requires -Version 5.1
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'setup.ps1')
& (Join-Path $PSScriptRoot 'build.ps1')
& (Join-Path $PSScriptRoot 'deploy.ps1')
