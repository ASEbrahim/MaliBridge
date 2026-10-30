#!/usr/bin/env python3
# imgsame.py a.png b.png: prints how many pixels differ by more than 8 in any channel and where; exit 0 when none do
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB"); b = Image.open(sys.argv[2]).convert("RGB")
if a.size != b.size:
    print("sizes differ", a.size, b.size); sys.exit(1)
d = ImageChops.difference(a, b).convert("L")
h = d.histogram()
n = sum(h[9:])
print("%d pixel(s) differ, box %s" % (n, d.point(lambda x: 255 if x > 8 else 0).getbbox()))
sys.exit(1 if n else 0)
