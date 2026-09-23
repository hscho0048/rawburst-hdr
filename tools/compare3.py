#!/usr/bin/env python3
"""사용: compare3.py out.png single.ppm merged.ppm bokeh.ppm [--crop x,y,w,h] [--scale 0.25]
단일 프레임 | N장 합성 | 합성+보케 3장을 나란히 (README 첫 그림)."""
import sys
from PIL import Image, ImageDraw
args = sys.argv[1:]
crop = None; scale = 0.25
if "--crop" in args: i = args.index("--crop"); crop = tuple(map(int, args[i+1].split(","))); del args[i:i+2]
if "--scale" in args: i = args.index("--scale"); scale = float(args[i+1]); del args[i:i+2]
out, paths = args[0], args[1:]
labels = ["single frame", "burst merge", "merge + bokeh"][:len(paths)]
ims = []
for p in paths:
    im = Image.open(p).convert("RGB")
    if crop: im = im.crop((crop[0], crop[1], crop[0] + crop[2], crop[1] + crop[3]))
    else: im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
    ims.append(im)
W = sum(i.width for i in ims) + 8 * (len(ims) - 1); H = max(i.height for i in ims) + 28
canvas = Image.new("RGB", (W, H), (20, 20, 20)); d = ImageDraw.Draw(canvas); x = 0
for im, lb in zip(ims, labels):
    canvas.paste(im, (x, 28)); d.text((x + 6, 8), lb, fill=(230, 230, 230)); x += im.width + 8
canvas.save(out); print("→", out)
