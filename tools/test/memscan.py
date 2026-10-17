"""Scan FlatOut2.exe's writable data in a running game for given int/float values.

usage: memscan.py <value> [<value> ...]      (ints; floats with the same value are also matched)
       memscan.py dump <va> <count>          dump dwords (hex, int, float)
"""
import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def pid_of(name):
    out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], text=True)
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == name.lower():
            return int(parts[1])
    raise SystemExit("game not running")


h = k32.OpenProcess(0x0010 | 0x0400, False, pid_of("FlatOut2.exe"))


def read(va, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(va), buf, n, ctypes.byref(got)):
        return None
    return buf.raw[: got.value]


if sys.argv[1] == "dump":
    va, n = int(sys.argv[2], 16), int(sys.argv[3])
    d = read(va, 4 * n)
    for i in range(n):
        v = struct.unpack_from("<I", d, 4 * i)[0]
        f = struct.unpack_from("<f", d, 4 * i)[0]
        print(f"{va + 4 * i:08X}  {v:08X}  {struct.unpack_from('<i', d, 4 * i)[0]:>11}  {f:g}")
    raise SystemExit

vals = [int(v) for v in sys.argv[1:]]
start, end = 0x681000, 0x904000
data = b""
for a in range(start, end, 0x1000):
    d = read(a, 0x1000)
    data += d if d else b"\0" * 0x1000
pats = {}
for v in vals:
    pats[struct.pack("<i", v)] = f"int {v}"
    pats[struct.pack("<f", float(v))] = f"float {v}"
for i in range(0, len(data) - 3, 4):
    w = data[i : i + 4]
    if w in pats:
        print(f"{start + i:08X}  {pats[w]}")
