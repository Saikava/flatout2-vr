"""Find float values inside a structure of the running game.
usage: memfind.py <base va hex> <size hex> <float> [<float> ...]"""
import ctypes, ctypes.wintypes as wt, struct, subprocess, sys
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq FlatOut2.exe", "/FO", "CSV", "/NH"], text=True)
pid = int(out.split('","')[1])
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
def read(va, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p(va), buf, n, ctypes.byref(got)); return buf.raw[:got.value]
def rd32(va): return struct.unpack("<I", read(va, 4))[0]
base = sys.argv[1]
if base.startswith("world"):
    gf = rd32(0x8E8410); race = rd32(gf + 0x9B4); obj = rd32(race + 0x10)
    print(f"GameFlow {gf:08X} race {race:08X} obj {obj:08X} world {obj + 0xA0:08X}")
    base = obj + 0xA0
else:
    base = int(base, 16)
size = int(sys.argv[2], 16)
d = b""
for a in range(base, base + size, 0x1000):
    chunk = read(a, min(0x1000, base + size - a)); d += chunk if chunk else b"\0" * min(0x1000, base + size - a)
for f in sys.argv[3:]:
    v = float(f)
    for i in range(0, len(d) - 3, 4):
        x = struct.unpack_from("<f", d, i)[0]
        if abs(x - v) <= abs(v) * 1e-5:
            print(f"{base + i:08X} (+{i:X})  {x:g}")
