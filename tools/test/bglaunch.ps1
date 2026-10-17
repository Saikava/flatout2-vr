# Background mode: starts the game (scripts\FO2VR.ini must have Background=1) and waits for its device.
$log = "C:\GOG Games\FlatOut 2\scripts\FO2VR.log"
if (Get-Process FlatOut2 -ErrorAction SilentlyContinue) { "already running"; return }
Start-Process -FilePath "C:\GOG Games\FlatOut 2\FlatOut2.exe" -WorkingDirectory "C:\GOG Games\FlatOut 2" -WindowStyle Minimized
Start-Sleep -Seconds 2
for ($i = 0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; if ((Test-Path $log) -and (Select-String -Path $log -Pattern "CreateDevice" -Quiet)) { break } }
Select-String -Path $log -Pattern "background|CreateDevice" | ForEach-Object { $_.Line }
