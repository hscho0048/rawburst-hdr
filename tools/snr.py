#!/usr/bin/env python3
"""사용: snr.py single.raw16 merged.raw16 W H x y w h [N]
       snr.py single.raw16 merged.raw16 --burst bursts/emu_static [N]   (truth.txt의 flat 영역 사용, 에뮬 세트)
평탄 영역 [x,y,w,h] 에서 같은 CFA 위상(2칸 간격)만 뽑아 표준편차 비율(dB)을 출력."""
import sys, os, numpy as np

a, b = sys.argv[1], sys.argv[2]
if sys.argv[3] == "--burst":
    d = sys.argv[4]
    kv = {l.split()[0]: l.split()[1:] for l in open(os.path.join(d, "meta.txt"))}
    W, H = int(kv["width"][0]), int(kv["height"][0])
    tr = {l.split()[0]: l.split()[1:] for l in open(os.path.join(d, "truth.txt"))}
    x, y, w, h = map(int, tr["flat"])
    n = int(sys.argv[5]) if len(sys.argv) > 5 else 8
else:
    W, H, x, y, w, h = map(int, sys.argv[3:9])
    n = int(sys.argv[9]) if len(sys.argv) > 9 else 8
x, y = x & ~1, y & ~1

def load(p): return np.fromfile(p, np.uint16).reshape(H, W).astype(np.float64)

s, m = load(a), load(b)
print(f"region x={x} y={y} w={w} h={h}")
gains = []
for py in range(2):
    for px in range(2):
        rs = s[y+py:y+h:2, x+px:x+w:2]; rm = m[y+py:y+h:2, x+px:x+w:2]
        g = 20 * np.log10(rs.std() / rm.std()); gains.append(g)
        print(f"  CFA ({px},{py}): single mean={rs.mean():.1f} std={rs.std():.2f} | merged mean={rm.mean():.1f} std={rm.std():.2f} | {g:.2f} dB")
print(f"SNR gain: {np.mean(gains):.2f} dB (이론 최대 {20*np.log10(np.sqrt(n)):.2f} dB @{n}장)")
