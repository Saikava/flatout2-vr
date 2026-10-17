# Background mode: records a short race in which the (test) head turns about, then closes the game.
$t = $PSScriptRoot
& "$t\bgrace.ps1"
& "$t\send.ps1" -Keys '+UP','WAIT1500','HEAD:0 0 0 35','WAIT2500','HEAD:0.1 -0.05 0 -50','WAIT2500','HEAD:0 0.1 0 80','WAIT2500','HEAD:0 0 0 0','+LEFT','WAIT900','-LEFT','WAIT3000','HEAD:0 0 0 -120','WAIT3000','-UP','HEAD:0 0 0 0','WAIT1500'
Start-Sleep -Seconds 1
Stop-Process -Name FlatOut2 -Force
"done"
