#!/usr/bin/env python3
"""PowerService CSV 샘플 간격 점검: 기기가 suspend로 가면 샘플 사이에 큰 구멍이 생긴다 (wake lock 무력화 확인용)."""
import csv
import sys

rows = list(csv.DictReader(open(sys.argv[1])))
t = [int(x["t_ns"]) for x in rows]
d = sorted((b - a) / 1e6 for a, b in zip(t, t[1:]))
print(f"n={len(rows)} span={(t[-1] - t[0]) / 1e9:.1f}s median_dt={d[len(d) // 2]:.1f}ms p90={d[len(d) * 9 // 10]:.1f}ms max={d[-1]:.0f}ms")
gaps = [((b - t[0]) / 1e9, (b - a) / 1e6) for a, b in zip(t, t[1:]) if (b - a) > 1e9]
print(f"gaps>1s: {len(gaps)} total={sum(g for _, g in gaps) / 1e3:.1f}s first={[(round(x, 1), round(g)) for x, g in gaps[:8]]}")
cur = [int(x["current_raw"]) for x in rows]
print("current_raw min/median/max:", min(cur), sorted(cur)[len(cur) // 2], max(cur), "voltage:", rows[0]["voltage_mv"], rows[-1]["voltage_mv"])
