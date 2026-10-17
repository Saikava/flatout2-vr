# Sends key presses to the game (scancodes, so DirectInput sees them).
# usage: keys.ps1 -Keys ENTER,ENTER,F11 [-DelayMs 800]
param([string[]]$Keys, [int]$DelayMs = 800)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Kbd {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
$map = @{ ENTER = @(0x0D, 0x1C); ESC = @(0x1B, 0x01); UP = @(0x26, 0x48); DOWN = @(0x28, 0x50); LEFT = @(0x25, 0x4B); RIGHT = @(0x27, 0x4D);
          F11 = @(0x7A, 0x57); F8 = @(0x77, 0x42); F5 = @(0x74, 0x3F); SPACE = @(0x20, 0x39) }
$p = Get-Process FlatOut2 -ErrorAction Stop
[Kbd]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
$fgPid = 0; [Kbd]::GetWindowThreadProcessId([Kbd]::GetForegroundWindow(), [ref]$fgPid) | Out-Null
if ($fgPid -ne $p.Id) { Write-Output "warning: game is not in the foreground (pid $fgPid)" }
foreach ($k in $Keys) {
  if ($k -match '^WAIT(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
  # +KEY holds the key down, -KEY releases it.
  $hold = $k.StartsWith('+'); $release = $k.StartsWith('-')
  $name = $k.TrimStart('+', '-')
  if ($map.ContainsKey($name)) { $vk, $sc = $map[$name] }
  elseif ($name -match '^[A-Z0-9]$') { $vk = [int][char]$name; $sc = [Kbd]::MapVirtualKey([uint32]$vk, 0) }
  elseif ($name -match '^VK([0-9A-F]{2})$') { $vk = [Convert]::ToInt32($Matches[1], 16); $sc = [Kbd]::MapVirtualKey([uint32]$vk, 0) }
  else { Write-Output "unknown key $name"; continue }
  $ext = if ($name -in 'UP','DOWN','LEFT','RIGHT') { 1 } else { 0 }
  if ($release) { [Kbd]::keybd_event([byte]$vk, [byte]$sc, [uint32](0x8 -bor 0x2 -bor $ext), [UIntPtr]::Zero); continue }
  [Kbd]::keybd_event([byte]$vk, [byte]$sc, [uint32](0x8 -bor $ext), [UIntPtr]::Zero)
  if ($hold) { continue }
  Start-Sleep -Milliseconds 80
  [Kbd]::keybd_event([byte]$vk, [byte]$sc, [uint32](0x8 -bor 0x2 -bor $ext), [UIntPtr]::Zero)
  Start-Sleep -Milliseconds $DelayMs
}
