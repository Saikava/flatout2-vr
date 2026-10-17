"""bfsgrep.py <pattern> [outdir]: decompress every file in the game's BFS archives and report/save those containing pattern."""
import os, re, struct, sys, zlib
pat = re.compile(sys.argv[1].encode(), re.I)
out = sys.argv[2] if len(sys.argv) > 2 else None
root = r"C:\GOG Games\FlatOut 2"
for arc in ("fo2a.bfs", "fo2b.bfs", "fo2c.bfs", "patch1.bfs"):
    p = os.path.join(root, arc)
    if not os.path.exists(p):
        continue
    d = open(p, "rb").read()
    magic, ver, hend, count = struct.unpack_from("<4sIII", d, 0)
    hsize = struct.unpack_from("<I", d, 0x10)[0]
    o = min(struct.unpack_from("<I", d, 0x14 + 8 * k)[0] for k in range(hsize) if struct.unpack_from("<I", d, 0x18 + 8 * k)[0])
    for i in range(count):
        flags, off, usize, psize, crc, a, b = struct.unpack_from("<IIIIIHH", d, o)
        o += 24
        if usize > 4 << 20:
            continue
        raw = d[off:off + psize]
        try:
            data = zlib.decompress(raw) if (flags & 0xff) == 5 else raw
        except Exception:
            continue
        if pat.search(data):
            name = "%s_%08x" % (arc, off)
            print(name, usize, data[:60])
            if out:
                os.makedirs(out, exist_ok=True)
                open(os.path.join(out, name + ".txt"), "wb").write(data)
