# Packs a release: FlatOut2-VR-<version>.zip, laid out like the game folder.
#   powershell -File tools\package.ps1 -Version 1.0 [-AsiLoader <winmm.dll>] [-Dxvk <d3d9.dll>] [-Out <folder>]
# The ASI loader (Ultimate ASI Loader, 32-bit, as winmm.dll) and DXVK's 32-bit d3d9.dll are taken from the game
# folder by default (the ones this mod is tested with); the mod from the last build (build.cmd).
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$AsiLoader,
    [string]$Dxvk,
    [string]$Out
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$game = Split-Path $repo
if (-not $AsiLoader) { $AsiLoader = Join-Path $game "winmm.dll" }
if (-not $Dxvk) { $Dxvk = Join-Path $game "d3d9.dll" }
if (-not $Out) { $Out = Join-Path $repo "build\release" }
$stage = Join-Path $Out "FlatOut2-VR-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\scripts\FO2VR-licenses" | Out-Null

Copy-Item $AsiLoader "$stage\winmm.dll"
Copy-Item $Dxvk "$stage\d3d9.dll"
Copy-Item "$repo\build\FO2VR.asi" "$stage\scripts\"
Copy-Item "$repo\build\wineopenxr.dll" "$stage\scripts\"
Copy-Item "$repo\third_party\openxr\bin\openxr_loader.dll" "$stage\scripts\"
Copy-Item "$repo\install\FO2VR.ini" "$stage\scripts\"
Copy-Item "$repo\README.md" "$stage\scripts\FO2VR-README.md"
Copy-Item "$repo\LICENSE" "$stage\scripts\FO2VR-licenses\FO2VR.txt"
Copy-Item "$repo\install\licenses\*" "$stage\scripts\FO2VR-licenses\"

$zip = Join-Path $Out "FlatOut2-VR-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-ChildItem -Recurse $stage | Where-Object { -not $_.PSIsContainer } | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
"-> $zip"
