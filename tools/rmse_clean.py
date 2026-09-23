#!/usr/bin/env python3
"""사용: rmse_clean.py merged.raw16 bursts/emu_xxx [ref]
에뮬 세트의 노이즈 없는 정답(clean_REF.raw16, --clean으로 생성)과 합성 결과의 RMSE를 영역별로:
전체 / 엣지(정답 그래디언트 상위 10%) / 평탄(하위 50%). ref 생략 시 0~2 중 전체 RMSE 최소인 프레임."""
import sys, os, numpy as np
m_path, d = sys.argv[1], sys.argv[2]
kv = {l.split()[0]: l.split()[1:] for l in open(os.path.join(d, "meta.txt"))}
W, H = int(kv["width"][0]), int(kv["height"][0])
m = np.fromfile(m_path, np.uint16).reshape(H, W).astype(np.float64)
def load(i): return np.fromfile(os.path.join(d, f"clean_{i:02d}.raw16"), np.uint16).reshape(H, W).astype(np.float64)
refs = [int(sys.argv[3])] if len(sys.argv) > 3 else [0, 1, 2]
best = min(refs, key=lambda i: np.mean((m - load(i)) ** 2))
c = load(best)
# 같은 CFA 위상 그래디언트 (2px 차)
g = np.zeros_like(c); g[:, 2:] += np.abs(c[:, 2:] - c[:, :-2]); g[2:, :] += np.abs(c[2:, :] - c[:-2, :])
b = 32; sl = (slice(b, H - b), slice(b, W - b))
e2 = (m - c)[sl] ** 2; gg = g[sl]
hi, lo = np.percentile(gg, 90), np.percentile(gg, 50)
print(f"ref={best} RMSE all={np.sqrt(e2.mean()):.3f} edge={np.sqrt(e2[gg >= hi].mean()):.3f} flat={np.sqrt(e2[gg <= lo].mean()):.3f} DN")
