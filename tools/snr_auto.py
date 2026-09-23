#!/usr/bin/env python3
"""사용: snr_auto.py single.raw16 merged.raw16 W H [black] [N]
실제 장면용 SNR 이득: 128×128 블록마다 같은 CFA 위상(Gr) 픽셀의 노이즈 σ를 강건 추정
(σ = 이웃 같은 위상 픽셀 차의 절사 std / √2, 완만한 그래디언트에 강함)하고, 텍스처가 적은(평탄) 블록 상위 10%에서
single/merged σ 비의 중앙값(dB)을 낸다. 밝기 구간별로도 출력."""
import sys, numpy as np
a, b = sys.argv[1], sys.argv[2]; W, H = int(sys.argv[3]), int(sys.argv[4])
black = float(sys.argv[5]) if len(sys.argv) > 5 else 64; N = int(sys.argv[6]) if len(sys.argv) > 6 else 8
s = np.fromfile(a, np.uint16).reshape(H, W).astype(np.float64)
m = np.fromfile(b, np.uint16).reshape(H, W).astype(np.float64)
def sig(p):  # p: 같은 위상 2D. 이웃 차의 5–95% 절사 표준편차/√2 (정수 raw에서 median 기반은 양자화돼 쓸 수 없다)
    d = (p[:, 1:] - p[:, :-1]).ravel()
    lo, hi = np.percentile(d, [5, 95]); d = d[(d >= lo) & (d <= hi)]
    return d.std() / np.sqrt(2) / 0.79   # 정규분포 5–95% 절사 std ≈ 0.79σ
rows = []
B = 128
for y in range(0, H - B, B):
    for x in range(0, W - B, B):
        ps = s[y:y+B:2, x+1:x+B:2]; pm = m[y:y+B:2, x+1:x+B:2]   # (x odd, y even) 위상
        mean = pm.mean() - black
        if mean < 4 or pm.max() >= 1000: continue            # 너무 어둡거나 포화 블록 제외
        tex = pm.std()                                          # 합성본 std ≈ 텍스처
        ss, sm = sig(ps), sig(pm)
        if sm <= 0: continue
        rows.append((tex / max(ss, 1e-6), mean, ss, sm))
rows.sort()
flat = rows[:max(5, len(rows) // 10)]
g = [20 * np.log10(r[2] / r[3]) for r in flat]
print(f"blocks={len(rows)} flat={len(flat)}  SNR gain median {np.median(g):.2f} dB (IQR {np.percentile(g,25):.2f}–{np.percentile(g,75):.2f}), 이론 {20*np.log10(np.sqrt(N)):.2f} dB")
for lo, hi in [(4, 15), (15, 40), (40, 120), (120, 960)]:
    sel = [20*np.log10(r[2]/r[3]) for r in rows[:len(rows)//3] if lo <= r[1] < hi]
    if len(sel) >= 3: print(f"  신호 {lo:>3}–{hi:<3} DN: {np.median(sel):.2f} dB (n={len(sel)})")
