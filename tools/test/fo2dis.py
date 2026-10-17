"""Tiny static helper for FlatOut2.exe: VA<->file mapping, disassembly, reads, xrefs.

usage:
  fo2dis.py dis  <va> [count]        disassemble count instructions (default 60)
  fo2dis.py fn   <va> [maxinsn]      disassemble until ret (or maxinsn)
  fo2dis.py dd   <va> [count]        dump dwords
  fo2dis.py ff   <va> [count]        dump floats
  fo2dis.py calls <target>           find E8 calls to target
  fo2dis.py refs <value>             find dword immediates/references equal to value
  fo2dis.py str  <text>              find ascii string, print VA and dword refs to it
"""
import struct
import sys

import capstone

EXE = r"C:\GOG Games\FlatOut 2\FlatOut2.exe"
data = open(EXE, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
optsz = struct.unpack_from("<H", data, pe + 20)[0]
base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = data[o:o + 8].rstrip(b"\0").decode()
    vsz, va, rsz, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((name, base + va, vsz, raw, rsz))


def off(va):
    for name, sva, vsz, raw, rsz in secs:
        if sva <= va < sva + max(vsz, rsz):
            d = va - sva
            return raw + d if d < rsz else None
    return None


def rd(va, n):
    o = off(va)
    return data[o:o + n] if o is not None else None


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def dis(va, count, stop_at_ret=False):
    code = rd(va, count * 15)
    n = 0
    for ins in md.disasm(code, va):
        print(f"{ins.address:08X}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
        n += 1
        if n >= count or (stop_at_ret and ins.mnemonic in ("ret", "retn")):
            break


def text_sections():
    return [s for s in secs if s[0] in (".text",) or s[0].startswith(".text")]


def main():
    cmd = sys.argv[1]
    a = int(sys.argv[2], 16) if len(sys.argv) > 2 and cmd not in ("str", "grep") else None
    if cmd == "secs":
        for s in secs:
            print(s[0], hex(s[1]), hex(s[2]), hex(s[3]), hex(s[4]))
    elif cmd == "dis":
        dis(a, int(sys.argv[3]) if len(sys.argv) > 3 else 60)
    elif cmd == "fn":
        dis(a, int(sys.argv[3]) if len(sys.argv) > 3 else 400, stop_at_ret=True)
    elif cmd == "dd":
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        for i in range(n):
            print(f"{a + 4 * i:08X}  {struct.unpack('<I', rd(a + 4 * i, 4))[0]:08X}")
    elif cmd == "ff":
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        for i in range(n):
            print(f"{a + 4 * i:08X}  {struct.unpack('<f', rd(a + 4 * i, 4))[0]:g}")
    elif cmd == "calls":
        for name, sva, vsz, raw, rsz in secs:
            if name != ".text":
                continue
            seg = data[raw:raw + rsz]
            i = seg.find(b"\xE8")
            while i != -1:
                if i + 5 <= len(seg):
                    rel = struct.unpack_from("<i", seg, i + 1)[0]
                    if (sva + i + 5 + rel) & 0xFFFFFFFF == a:
                        print(f"call at {sva + i:08X}")
                i = seg.find(b"\xE8", i + 1)
    elif cmd == "refs":
        pat = struct.pack("<I", a)
        i = data.find(pat)
        while i != -1:
            for name, sva, vsz, raw, rsz in secs:
                if raw <= i < raw + rsz:
                    print(f"{name} {sva + i - raw:08X}")
            i = data.find(pat, i + 1)
    elif cmd == "grep":
        import re
        rx = re.compile(sys.argv[2].encode("latin1").decode("unicode_escape").encode("latin1"), re.S)
        for name, sva, vsz, raw, rsz in text_sections():
            for m in rx.finditer(data[raw:raw + rsz]):
                print(f"{sva + m.start():08X}  {m.group(0).hex()}")
    elif cmd == "str":
        s = sys.argv[2].encode()
        i = data.find(s)
        while i != -1:
            for name, sva, vsz, raw, rsz in secs:
                if raw <= i < raw + rsz:
                    va = sva + i - raw
                    print(f"string {name} {va:08X}: {data[i:i + 60].split(b'\0')[0]!r}")
                    pat = struct.pack("<I", va)
                    j = data.find(pat)
                    while j != -1:
                        for n2, sva2, _, raw2, rsz2 in secs:
                            if raw2 <= j < raw2 + rsz2:
                                print(f"   ref {n2} {sva2 + j - raw2:08X}")
                        j = data.find(pat, j + 1)
            i = data.find(s, i + 1)


if __name__ == "__main__":
    main()
