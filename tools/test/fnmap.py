"""fnmap.py <prof.txt>: groups profile places by the function they fall in (nearest call target below)."""
import bisect, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fo2dis
targets = set()
for name, sva, vsz, raw, rsz in fo2dis.secs:
    if not name.startswith('.text'): continue
    code = fo2dis.data[raw:raw + rsz]
    i = code.find(b'\xe8')
    while i >= 0:
        t = sva + i + 5 + struct.unpack_from('<i', code, i + 1)[0]
        if sva <= t < sva + rsz: targets.add(t)
        i = code.find(b'\xe8', i + 1)
targets = sorted(targets)
fn = {}
total = 0
for line in open(sys.argv[1]):
    m = re.match(r'\s*(game|called from) ([0-9A-F]+) (\d+)', line)
    if not m: continue
    a, n = int(m.group(2), 16), int(m.group(3))
    total += n
    f = targets[bisect.bisect_right(targets, a) - 1]
    key = (m.group(1), f)
    fn[key] = fn.get(key, 0) + n
for (kind, f), n in sorted(fn.items(), key=lambda x: -x[1]):
    print(f"{n:6d} {100*n/total:5.1f}%  {kind:11s} fn {f:06X}")
