import glob, sys
from PIL import Image
t = sys.argv[1]
out = sys.argv[2]
l = Image.open(glob.glob(f'fo2vr_{t}_*_vr_left.png')[0]).convert('RGB')
r = Image.open(glob.glob(f'fo2vr_{t}_*_vr_right.png')[0]).convert('RGB')
w, h = l.size; s = float(sys.argv[3]) if len(sys.argv) > 3 else 0.35
c = Image.new('RGB', (int(w*s)*2+10, int(h*s)), (255, 0, 255))
c.paste(l.resize((int(w*s), int(h*s))), (0, 0)); c.paste(r.resize((int(w*s), int(h*s))), (int(w*s)+10, 0))
c.save(out)
