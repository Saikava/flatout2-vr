# Background mode ([Test] Background=1): sends keys to the game through scripts\fo2vr_input.txt
# (the mod feeds them to DirectInput), never through the real keyboard or focus.
# usage: send.ps1 -Keys ENTER,WAIT3000,+W,WAIT5000,-W,SHOT [-DelayMs 800]
#   KEY = tap, +KEY = hold, -KEY = release, WAITms = pause, SHOT = screenshot, LOG:text = marker in FO2VR.log, HEAD:x y z yaw [pitch] = test head pose, CAMERA:x y z = [VR] CameraOffset,
#   HAND:hand x y z yaw pitch [trigger [stick-x stick-y [button]]] (or HAND:hand off) = test controller (FakeHeadset)
#   EJECT = the player's driver thrown out of the car, TRACE = log one frame of render calls
#   MIRROR:l|r loose|off = knock the player's car's mirror loose or off
param([string[]]$Keys, [int]$DelayMs = 800)
$file = "C:\GOG Games\FlatOut 2\scripts\fo2vr_input.txt"
function Put([string]$line) { [System.IO.File]::AppendAllText($file, $line + "`n") }
foreach ($k in $Keys) {
  if ($k -match '^WAIT(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
  if ($k -eq 'SHOT') { Put 'shot'; continue }
  if ($k -eq 'EJECT') { Put 'eject'; continue }
  if ($k -eq 'TRACE') { Put 'trace'; continue }
  if ($k.StartsWith('LOG:')) { Put ('log ' + $k.Substring(4)); continue }
  if ($k.StartsWith('HEAD:')) { Put ('head ' + $k.Substring(5)); continue }
  if ($k.StartsWith('CAMERA:')) { Put ('camera ' + $k.Substring(7)); continue }
  if ($k.StartsWith('HAND:')) { Put ('hand ' + $k.Substring(5)); continue }
  if ($k.StartsWith('MIRROR:')) { Put ('mirror ' + $k.Substring(7)); continue }
  if ($k.StartsWith('+')) { Put ('down ' + $k.Substring(1)); continue }
  if ($k.StartsWith('-')) { Put ('up ' + $k.Substring(1)); continue }
  Put ('tap ' + $k)
  Start-Sleep -Milliseconds $DelayMs
}
