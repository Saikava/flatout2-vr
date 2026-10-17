"""Print the local player's car position, velocity and speed (km/h) in the running game."""
import ctypes, ctypes.wintypes as wt, struct, subprocess, math
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq FlatOut2.exe", "/FO", "CSV", "/NH"], text=True)
h = k32.OpenProcess(0x0010 | 0x0400, False, int(out.split('","')[1]))
def read(va, n):
    buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
    k32.ReadProcessMemory(h, ctypes.c_void_p(va), buf, n, ctypes.byref(got)); return buf.raw[:got.value]
def u32(va): return struct.unpack("<I", read(va, 4))[0]
car = u32(u32(0x6B21F8) + 0x33C)
m = struct.unpack("<16f", read(car + 0x1B0, 64)); v = struct.unpack("<3f", read(car + 0x280, 12))
print(f"car {car:08X} pos {m[12]:.1f} {m[13]:.1f} {m[14]:.1f} up.y {m[5]:.2f} speed {math.sqrt(sum(x*x for x in v))*3.6:.1f} km/h")
