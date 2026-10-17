import struct, sys
sys.argv = ["x", "secs"]
exec(open("fo2dis.py").read().split("def main():")[0])
for idx, label in ((1, "import"), (13, "delay-import")):
    rva, size = struct.unpack_from("<II", data, pe + 24 + 96 + 8 * idx)
    print(label, hex(rva), size)
    if not rva: continue
    o = off(base + rva)
    while True:
        if idx == 1:
            oft, ts, fwd, name, ft = struct.unpack_from("<IIIII", data, o); o += 20
        else:
            attrs, name = struct.unpack_from("<II", data, o); o += 32
        if not name: break
        print("  ", data[off(base + name):].split(b"\0")[0].decode())
