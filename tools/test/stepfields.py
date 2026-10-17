"""Print the physics world's step fields and the player's car step fields in the running game."""
import ctypes, ctypes.wintypes as wt, struct, subprocess
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq FlatOut2.exe", "/FO", "CSV", "/NH"], text=True)
h = k32.OpenProcess(0x0010 | 0x0400, False, int(out.split('","')[1]))
def read(va, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p(va), buf, n, ctypes.byref(got)); return buf.raw[:got.value]
def u32(va): return struct.unpack("<I", read(va, 4))[0]
def f32(va): return struct.unpack("<f", read(va, 4))[0]
gf = u32(0x8E8410)
sim = u32(u32(gf + 0x9B4) + 0x10)
w = sim + 0xA0
car = u32(u32(0x6B21F8) + 0x33C)
for i in range(5):
    print(f"world step {f32(w+0x20724)*1000:.4f} ms, inv {f32(w+0x20728):.3f}, ms {u32(w+0x2072C)} | car step {f32(car+0x1E08)*1000:.4f} ms, inv {f32(car+0x1E0C):.3f} | frame dt {u32(gf+0x9AC)}")
