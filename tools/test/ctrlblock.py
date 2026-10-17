import ctypes, ctypes.wintypes as wt, struct, subprocess, time
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
player = u32(0x6B21F8)
base, n = 0x640, 0xA0
rows = []
for keys, label in ((["up W"], "idle"), (["down S"], "S"), (["up S", "down SPACE"], "SPACE"), (["up SPACE", "down LSHIFT"], "LSHIFT"),
                    (["up LSHIFT", "down N"], "N"), (["up N", "down LCTRL"], "LCTRL"), (["up LCTRL"], "idle2")):
    send(*keys); time.sleep(0.5)
    rows.append((label, read(player + base, n)))
print("off   " + " ".join("%8s" % l for l, _ in rows))
for i in range(0, n, 4):
    vals = [r[i:i+4] for _, r in rows]
    if len(set(vals)) > 1:
        fs = [struct.unpack("<f", v)[0] for v in vals]; us = [struct.unpack("<I", v)[0] for v in vals]
        print("+%03X " % (base + i) + " ".join("%8.3f" % f if abs(f) < 1e6 and f == f else "%08X" % u for f, u in zip(fs, us)))
