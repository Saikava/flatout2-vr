# xpadstep.ps1 -GamePid N -Steps "pad:press START 150","game:shot","sleep:500",... [-Game <game folder>]
# Puts the game's window in front (as the mod's FocusGameWindow does: the input of the window in front shared for the
# moment), then runs the steps while it stays in front: pad: lines go to the virtual Xbox controller (xpad.py), game:
# lines to the game's background-mode input file (as send.ps1's). Stops when the game is no longer in front, and gives
# the focus back to the window that had it. While the game is in front the keyboard's keys go to it: say so first.
param([int]$GamePid, [string[]]$Steps, [string]$Game = "A:\SteamLibrary\steamapps\common\FlatOut 2")
$in = "$Game\scripts\fo2vr_input.txt"
$pc = Join-Path $PSScriptRoot "..\..\build\xpad_cmds.txt"
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Fg {
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 public static uint FrontPid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
 public static void Front(IntPtr w) {
  IntPtr f = GetForegroundWindow(); uint p; uint t = GetWindowThreadProcessId(f, out p); uint me = GetCurrentThreadId();
  bool a = t != 0 && t != me && AttachThreadInput(me, t, true);
  SetForegroundWindow(w); BringWindowToTop(w);
  if (a) AttachThreadInput(me, t, false);
 }
}
"@
$before = [Fg]::GetForegroundWindow()
$hw = (Get-Process -Id $GamePid).MainWindowHandle
[IO.File]::AppendAllText($pc, "fg $GamePid`n")
[Fg]::Front($hw); Start-Sleep -Milliseconds 300
if ([Fg]::FrontPid() -ne $GamePid) { "the game did not come in front"; return }
foreach ($s in $Steps) {
  if ([Fg]::FrontPid() -ne $GamePid) { "the game lost the front at: $s"; break }
  $k, $v = $s -split ':', 2
  switch ($k) {
    'pad' { [IO.File]::AppendAllText($pc, $v + "`n") }
    'game' { [IO.File]::AppendAllText($in, $v + "`n") }
    'sleep' { Start-Sleep -Milliseconds ([int]$v) }
  }
}
Start-Sleep -Milliseconds 300
[Fg]::Front($before)
"done; in front now: pid $([Fg]::FrontPid())"
