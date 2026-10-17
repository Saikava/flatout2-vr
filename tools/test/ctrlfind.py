"""Find the player's control values: snapshots of the player and car objects under different keys."""
import ctypes, ctypes.wintypes as wt, struct, subprocess, time, os
k32 = ctypes.WinDLL("kernel32"); k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq FlatOut2.exe", "/FO", "CSV", "/NH"], text=True)
h = k32.OpenProcess(0x0010 | 0x0400, False, int(out.split('","')[1]))
def read(va, n):
    b = ctypes.create_string_buffer(n); g = ctypes.c_size_t(); k32.ReadProcessMemory(h, ctypes.c_void_p(va), b, n, ctypes.byref(g)); return b.raw[:g.value]
def u32(va): return struct.unpack("<I", read(va, 4))[0]
F = r"C:\GOG Games\FlatOut 2\scripts\fo2vr_input.txt"
def send(*lines):
    with open(F, "a") as f:
        for l in lines: f.write(l + "\n")
player = u32(0x6B21F8); car = u32(player + 0x33C)
print("player %08X car %08X" % (player, car))
regions = {"player": (player, 0x1000), "car": (car, 0x9400)}
def snap():
    return {k: read(a, n) for k, (a, n) in regions.items()}
states = []
for keys, label in ((["up W", "up A", "up D"], "idle"), (["down W"], "W"), (["down A"], "W+A"), (["up A", "down D"], "W+D"), (["up D", "up W"], "idle2")):
    send(*keys); time.sleep(0.6)
    states.append((label, snap()))
for k, (a, n) in regions.items():
    vals = [struct.unpack("<%df" % (n // 4), s[k][: n // 4 * 4]) for _, s in states]
    for i in range(n // 4):
        v = [x[i] for x in vals]
        idle, w, wa, wd, idle2 = v
        if any(x != x for x in v): continue
        if abs(idle) < 0.05 and abs(idle2) < 0.05 and abs(w) > 0.5 and abs(w) <= 1.01:
            print("%s+%04X throttle-like: %s" % (k, i * 4, ["%.3f" % x for x in v]))
        if abs(w) < 0.05 and abs(wa) > 0.3 and abs(wd) > 0.3 and wa * wd < 0 and abs(wa) <= 1.01:
            print("%s+%04X steer-like: %s" % (k, i * 4, ["%.3f" % x for x in v]))
