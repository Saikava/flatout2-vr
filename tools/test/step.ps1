# Sends keys to the game, then takes a screenshot (F11) and writes a downscaled copy to menu.png beside these tools' folder.
param([string[]]$Keys = @(), [int]$DelayMs = 700, [int]$Width = 960)
$t = $PSScriptRoot
$shots = "C:\GOG Games\FlatOut 2\scripts\shots"
$before = (Get-ChildItem $shots -File | Sort-Object LastWriteTime | Select-Object -Last 1).Name
& "$t\keys.ps1" -Keys ($Keys + @('WAIT600', 'F11')) -DelayMs $DelayMs
for ($i = 0; $i -lt 20; $i++) {
  Start-Sleep -Milliseconds 500
  $f = Get-ChildItem $shots -File -Filter "*normal.png" | Sort-Object LastWriteTime | Select-Object -Last 1
  if ($f.Name -ne $before) { break }
}
Start-Sleep -Milliseconds 800
python -c "from PIL import Image; im=Image.open(r'$($f.FullName)').convert('RGB'); im.resize(($Width, $Width*im.size[1]//im.size[0])).save(r'$t\..\menu.png')"
$f.Name
