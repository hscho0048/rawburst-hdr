#!/usr/bin/env python3
"""사용: mask_iou.py alpha.pgm gt.pgm [mask256.bin]
cli --dump-alpha 결과(1/4 해상도)와 에뮬 GT(alpha_gt_q.pgm)의 IoU. mask256을 주면 업샘플만 한 원본 마스크 IoU도 함께."""
import sys, numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]), np.float32) / 255
g = np.asarray(Image.open(sys.argv[2]), np.float32) / 255
def iou(p, t): p, t = p > 0.5, t > 0.5; return (p & t).sum() / max(1, (p | t).sum())
def boundary_err(p, t):  # GT 경계 ±4px 안의 평균 |α−GT|
    edge = np.zeros_like(t, bool); tb = t > 0.5
    for dy in range(-4, 5):
        for dx in range(-4, 5): edge |= np.roll(np.roll(tb, dy, 0), dx, 1) != tb
    return float(np.abs(p - t)[edge].mean())
print(f"guided alpha : IoU={iou(a, g):.4f} boundary |Δα|={boundary_err(a, g):.3f}")
if len(sys.argv) > 3:
    m = np.fromfile(sys.argv[3], np.float32).reshape(256, 256)
    up = np.asarray(Image.fromarray(m).resize((g.shape[1], g.shape[0]), Image.BILINEAR), np.float32)
    print(f"raw mask up  : IoU={iou(up, g):.4f} boundary |Δα|={boundary_err(up, g):.3f}")
