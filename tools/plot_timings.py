#!/usr/bin/env python3
"""사용: plot_timings.py L0=out/t_L0.json L1=out/t_L1.json ...  → 마크다운 표 stdout
"*_parallel" 키는 임계 경로 밖(세그 스레드)이라 total에서 뺀다."""
import sys, json
rows = [(a.split("=", 1)[0], json.load(open(a.split("=", 1)[1]))) for a in sys.argv[1:]]
keys = []
for _, d in rows:
    for k in d:
        if k not in keys: keys.append(k)
print("| 설정 | " + " | ".join(keys) + " | total |")
print("|---" * (len(keys) + 2) + "|")
for name, d in rows:
    tot = sum(v for k, v in d.items() if not k.endswith("_parallel") and "." not in k)
    print(f"| {name} | " + " | ".join(f"{d[k]:.1f}" if k in d else "" for k in keys) + f" | {tot:.1f} |")
