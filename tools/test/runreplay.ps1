# Runs a replay (ini already set for it) and waits for the game to close itself (ReplayStop).
param([int]$TimeoutS = 300)
$exe = "C:\GOG Games\FlatOut 2\FlatOut2.exe"
if (Get-Process FlatOut2 -ErrorAction SilentlyContinue) { "already running"; return }
$p = Start-Process -FilePath $exe -WorkingDirectory "C:\GOG Games\FlatOut 2" -WindowStyle Minimized -PassThru
if (-not $p.WaitForExit($TimeoutS * 1000)) { Stop-Process -Id $p.Id -Force; "timeout" } else { "exited" }
