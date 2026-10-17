# Background mode: launches the game and goes to the start of a single race (default track and car).
$t = $PSScriptRoot
$log = "C:\GOG Games\FlatOut 2\scripts\FO2VR.log"
& "$t\bglaunch.ps1" | Out-Null
Start-Sleep -Seconds 5
& "$t\send.ps1" -Keys ENTER,ENTER,ENTER,ENTER,ENTER -DelayMs 3000   # intro videos, title
& "$t\send.ps1" -Keys ENTER -DelayMs 2500                           # profile
& "$t\send.ps1" -Keys ENTER,RIGHT -DelayMs 1500                     # single player, single race
& "$t\send.ps1" -Keys ENTER,ENTER,ENTER,ENTER -DelayMs 2500         # track, car, ... race
for ($i = 0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; if (Select-String -Path $log -Pattern "race state" -Quiet) { break } }
Start-Sleep -Seconds 2
& "$t\send.ps1" -Keys ENTER -DelayMs 300                            # start the race
"race started"
