"""List imports of FlatOut2.exe matching a substring: imports.py [filter]"""
import struct, sys
data = open(r"C:\GOG Games\FlatOut 2\FlatOut2.exe", "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((va, max(vsz, rsz), raw))
def off(rva):
    for va, sz, raw in secs:
        if va <= rva < va + sz: return rva - va + raw
    raise ValueError(hex(rva))
imp_rva = struct.unpack_from("<I", data, pe + 24 + 104)[0]
flt = sys.argv[1].lower() if len(sys.argv) > 1 else ""
o = off(imp_rva)
while True:
    oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", data, o)
    if name_rva == 0: break
    dll = data[off(name_rva):].split(b"\0")[0].decode()
    t = off(oft or ft); k = 0
    while True:
        v = struct.unpack_from("<I", data, t + 4 * k)[0]
        if v == 0: break
        if not v & 0x80000000:
            n = data[off(v) + 2:].split(b"\0")[0].decode()
            if flt in n.lower(): print(f"{dll} {n} IAT {base + ft + 4 * k:08X}")
        k += 1
    o += 20
