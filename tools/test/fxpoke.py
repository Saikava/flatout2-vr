"""Read (and optionally write) the game's spark/streak effect settings.
usage: fxpoke.py            -> print
       fxpoke.py boost      -> many long-lived sparks (for screenshots)"""
import ctypes, ctypes.wintypes as wt, struct, subprocess, sys
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq FlatOut2.exe", "/FO", "CSV", "/NH"], text=True)
h = k32.OpenProcess(0x0010 | 0x0020 | 0x0008 | 0x0400, False, int(out.split('","')[1]))
def read(va, n):
    b = ctypes.create_string_buffer(n); g = ctypes.c_size_t(); k32.ReadProcessMemory(h, ctypes.c_void_p(va), b, n, ctypes.byref(g)); return b.raw[:g.value]
def write(va, raw):
    g = ctypes.c_size_t(); k32.WriteProcessMemory(h, ctypes.c_void_p(va), raw, len(raw), ctypes.byref(g)); return g.value
spark = [("CountPerHitVelocity", 0x8DDCF8, "f"), ("MinCount", 0x8DDCFC, "i"), ("MaxCount", 0x8DDD00, "i"), ("Size", 0x8DDD04, "ff"),
         ("Life", 0x8DDD0C, "ff"), ("Gravity", 0x8DDD14, "f"), ("AngleSpeed", 0x8DDD18, "f")]
streak = [("AmountScale", 0x8DDD24, "f"), ("Size", 0x8DDD28, "ff"), ("Life", 0x8DDD30, "ff")]
if len(sys.argv) > 1 and sys.argv[1] == "boost":
    write(0x8DDCFC, struct.pack("<i", 60)); write(0x8DDD00, struct.pack("<i", 200))
    write(0x8DDD0C, struct.pack("<ff", 2.5, 3.5)); write(0x8DDD30, struct.pack("<ff", 2.5, 3.5))
for name, items in (("spark", spark), ("streak", streak)):
    for n, a, f in items:
        print(name, n, struct.unpack("<" + f, read(a, struct.calcsize("<" + f))))
