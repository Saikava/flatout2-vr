# Starts the game and drives the menus into a single race (Forest / Timberlands 1, first car).
# Watches FO2VR.log for the menu states instead of relying on fixed waits where possible.
$t = $PSScriptRoot
$log = "C:\GOG Games\FlatOut 2\scripts\FO2VR.log"
Start-Process -FilePath "C:\GOG Games\FlatOut 2\FlatOut2.exe" -WorkingDirectory "C:\GOG Games\FlatOut 2"
# Wait for the device (the intro video starts right after).
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Milliseconds 500
  if ((Test-Path $log) -and (Select-String -Path $log -Pattern "CreateDevice" -Quiet)) { break }
}
Start-Sleep -Seconds 4
& "$t\keys.ps1" -Keys ENTER,WAIT3000,ENTER,WAIT3000,ENTER,WAIT3000,ENTER,WAIT4000,RIGHT,WAIT1200,ENTER,WAIT2500,ENTER,WAIT2500,ENTER,WAIT2500,ENTER -DelayMs 300
# Wait for the race.
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Milliseconds 500
  if (Select-String -Path $log -Pattern "race state" -Quiet) { break }
}
Select-String -Path $log -Pattern "race state" | Select-Object -First 1 | ForEach-Object { $_.Line }
