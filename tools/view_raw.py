#!/usr/bin/env python3
"""사용: view_raw.py bursts/burst_xxx [frame_idx] [out.png] → PNG (2x2 슈퍼픽셀 간이 디모자이크, WB, 감마)"""
import sys, os, numpy as np
from PIL import Image

d = sys.argv[1]; i = int(sys.argv[2]) if len(sys.argv) > 2 else 0
out = sys.argv[3] if len(sys.argv) > 3 else f"frame_{i:02d}.png"
meta = {}
for line in open(os.path.join(d, "meta.txt")):
    k, *v = line.split(); meta.setdefault(k, []).append(v)
w, h = int(meta["width"][0][0]), int(meta["height"][0][0])
cfa = int(meta["cfa"][0][0]); white = float(meta["white_level"][0][0])
black = np.array(meta["black_level"][0], float)            # (y&1)*2+(x&1) 순서
wb = np.array(meta["wb_gains"][0], float) if "wb_gains" in meta else np.ones(4)
raw = np.fromfile(os.path.join(d, f"frame_{i:02d}.raw16"), np.uint16).reshape(h, w).astype(np.float32)
color = [[0, 1, 2, 3], [1, 0, 3, 2], [2, 3, 0, 1], [3, 2, 1, 0]][cfa]  # 위치 (y&1)*2+(x&1) → 0R 1Gr 2Gb 3B
ch = {}
for pos in range(4):
    py, px = pos // 2, pos % 2
    ch[color[pos]] = (raw[py::2, px::2] - black[pos]) / (white - black[pos])
rgb = np.stack([ch[0] * wb[0], (ch[1] * wb[1] + ch[2] * wb[2]) / 2, ch[3] * wb[3]], -1)
rgb = np.clip(rgb * 2.8, 0, 1) ** (1 / 2.2)
Image.fromarray((rgb * 255).astype(np.uint8)).save(out)
print("mean raw", raw.mean(), "→", out)
