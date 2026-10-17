# Find reads of DeviceD3d+8 / +0xC (screen width/height) shortly after the device pointer is loaded from 0x8DA718.
import sys
sys.argv = ["x", "secs"]
src = open("fo2dis.py").read().replace("\nmain()", "\n")
exec(src)
from capstone import x86
md.detail = True
name, sva, vsz, raw, rsz = [s for s in secs if s[0] == ".text"][0]
code = data[raw:raw + rsz]
window = {}
hits = []
for ins in md.disasm(code, sva):
    ops = ins.operands
    # mov reg, [0x8da718]
    if ins.mnemonic == "mov" and len(ops) == 2 and ops[0].type == x86.X86_OP_REG and ops[1].type == x86.X86_OP_MEM and ops[1].mem.base == 0 and ops[1].mem.disp == 0x8DA718:
        window[ops[0].reg] = 12
        continue
    for r in list(window):
        window[r] -= 1
        if window[r] <= 0:
            del window[r]
    for op in ops:
        if op.type == x86.X86_OP_MEM and op.mem.base in window and op.mem.disp in (8, 0xC) and op.mem.index == 0:
            hits.append(f"{ins.address:08X}  {ins.mnemonic} {ins.op_str}")
    # a write to a tracked reg ends tracking
    if len(ops) >= 1 and ops[0].type == x86.X86_OP_REG and ops[0].reg in window and ins.mnemonic not in ("push", "cmp", "test"):
        del window[ops[0].reg]
print("\n".join(hits))
