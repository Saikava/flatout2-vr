# Background mode: records ~40 s of a race driven into the scenery while the (test) head turns about.
$t = $PSScriptRoot
& "$t\bgrace.ps1"
$k = @('+UP')
$yaws = @(40, -60, 90, -30, 150, 0, -100, 60, -20, 120, -80, 30, 0, 70, -45, 10, -130, 50, 0, 25)
for ($i = 0; $i -lt $yaws.Count; $i++) {
  $steer = if ($i % 2 -eq 0) { 'LEFT' } else { 'RIGHT' }
  $k += @("HEAD:0 0 0 $($yaws[$i])", "+$steer", 'WAIT700', "-$steer", 'WAIT1300')
}
$k += @('-UP', 'HEAD:0 0 0 0', 'WAIT1000')
& "$t\send.ps1" -Keys $k
Stop-Process -Name FlatOut2 -Force
"done"
