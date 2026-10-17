"""setini.py Key=Value ...: set keys in scripts/FO2VR.ini, keeping the comments."""
import re, sys
p = r"C:\GOG Games\FlatOut 2\scripts\FO2VR.ini"
s = open(p, encoding="utf-8").read()
for kv in sys.argv[1:]:
    k, v = kv.split("=", 1)
    m = re.search(r"(?m)^(%s=)([^;\r\n]*?)(\s*(;.*)?)$" % re.escape(k), s)
    assert m, k
    val = v + (" " * max(1, len(m.group(2)) + len(m.group(3)) - len(v) - len(m.group(3).lstrip())) if m.group(4) else "")
    s = s[:m.start()] + m.group(1) + (val + m.group(3).lstrip() if m.group(4) else v) + s[m.end():]
open(p, "w", encoding="utf-8", newline="").write(s)
