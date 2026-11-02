"""Contact sheet of the bench's pictures: python3 sheet.py <out dir> <sheet.png> [columns] [name filter]"""
import glob, os, sys
from PIL import Image, ImageDraw
d, outp = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 5
flt = sys.argv[4] if len(sys.argv) > 4 else ""
files = sorted(f for f in glob.glob(os.path.join(d, "*.pgm")) if flt in os.path.basename(f))
w, h, s = 480, 800, 0.5
tw, th = int(w * s), int(h * s) + 18
rows = (len(files) + cols - 1) // cols
sheet = Image.new("L", (cols * (tw + 8) + 8, rows * (th + 8) + 8), 150)
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert("L").resize((tw, int(h * s)), Image.LANCZOS)
    x, y = 8 + (i % cols) * (tw + 8), 8 + (i // cols) * (th + 8)
    sheet.paste(im, (x, y + 18)); dr.text((x, y + 2), os.path.basename(f)[:-4], fill=0)
sheet.save(outp)
print(outp, len(files))
