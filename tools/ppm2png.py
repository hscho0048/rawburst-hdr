#!/usr/bin/env python3
"""사용: ppm2png.py a.ppm [b.pgm ...] → 같은 이름 .png"""
import sys
from PIL import Image
for p in sys.argv[1:]:
    Image.open(p).save(p.rsplit(".", 1)[0] + ".png"); print(p, "→ png")
