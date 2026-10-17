"""Find instructions whose memory operand has a given displacement: dispscan.py <disp hex> [mnemonic regex]"""
import re, sys
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo2dis
import capstone
disp = int(sys.argv[1], 16)
mre = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
pat = disp.to_bytes(4, 'little')
for name, sva, vsz, raw, rsz in fo2dis.secs:
    if not name.startswith('.text'):
        continue
    code = fo2dis.data[raw:raw + rsz]
    i = code.find(pat)
    while i >= 0:
        for back in range(2, 8):
            s = i - back
            for ins in md.disasm(code[s:s + 15], sva + s):
                if ins.address == sva + s and ins.size >= back + 4 and f"0x{disp:x}" in ins.op_str and (not mre or mre.search(ins.mnemonic)):
                    print(f"{ins.address:08X}  {ins.mnemonic} {ins.op_str}")
                break
        i = code.find(pat, i + 1)
