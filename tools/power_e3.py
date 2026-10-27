#!/usr/bin/env python3
"""E3(150연속 촬영) 세부: 장 구간별 처리 ms·셔터→JPEG·합성 프레임 수(N), 스로틀 진입 시점(클럭 상한 하락), 구간별 에너지.
  python tools/power_e3.py results/power/<dev>/raw/<E3 run> [--calib results/power/<dev>/calibration.json]
monitor.txt: uptime cpu0 cpu4 cpu7 cap4 cap7 cpu_max_mC (1초 주기, 기기 쉘). 샷 시각은 CLOCK_BOOTTIME(ns) — /proc/uptime과 같은 시간축."""
import argparse
import glob
import json
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(__file__))
import power_analyze as pa  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("raw")
ap.add_argument("--calib")
a = ap.parse_args()
mon = []
mp = os.path.join(a.raw, "monitor.txt")
if os.path.exists(mp):
    for ln in open(mp):
        p = ln.split()
        if len(p) == 7:
            try:
                mon.append([float(p[0])] + [int(x) for x in p[1:]])
            except ValueError:
                pass
rows = pa.load_csv([os.path.join(a.raw, "samples.csv")]) if a.calib else None
calib = json.load(open(a.calib)) if a.calib else None
out = {}
for f in sorted(glob.glob(os.path.join(a.raw, "E3_r*.json"))):
    if f.endswith("_idle.json"):
        continue
    d = json.load(open(f))
    shots = d.get("shots", [])
    if not shots:
        continue
    t0 = d["t0_ns"] / 1e9
    cap_max = max((m[4] for m in mon if t0 - 5 <= m[0] <= d["t1_ns"] / 1e9), default=0)
    onset = next((m for m in mon if m[0] >= t0 and m[4] < cap_max), None)  # big 코어 상한이 처음 내려간 때
    deep = next((m for m in mon if m[0] >= t0 and m[4] <= 0.75 * cap_max), None)  # 2단계: 상한이 최대의 75% 이하
    shot_at = lambda ev: next((i + 1 for i, s in enumerate(shots) if s["t_ns"] / 1e9 >= ev[0]), None) if ev else None
    onset_shot, deep_shot = shot_at(onset), shot_at(deep)
    bins = []
    for lo in range(0, len(shots), 30):
        b = shots[lo:lo + 30]
        e = None
        if rows:
            ta = (b[0]["t_ns"] - int(b[0]["wall"] * 1e6))
            r = pa.integrate(rows, ta, b[-1]["t_ns"], calib)
            e = round(r["gross_j"] / len(b), 3) if r else None
        bins.append({"shots": f"{lo + 1}-{lo + len(b)}", "proc_ms_med": st.median(s["proc"] for s in b),
                     "wall_ms_med": st.median(s["wall"] for s in b), "frames_mean": round(st.mean(s["t"].get("frames", 8) for s in b), 2),
                     "gross_j_per_shot": e})
    cap_min = min((m[4] for m in mon if t0 <= m[0] <= d["t1_ns"] / 1e9), default=0)
    peak_c = max((m[6] for m in mon if t0 <= m[0] <= d["t1_ns"] / 1e9), default=0) / 1000
    out[d["label"]] = {"policy": d.get("policy"), "throttle_onset_shot": onset_shot,
                       "throttle_onset_s": round(onset[0] - t0, 1) if onset else None,
                       "deep_throttle_shot": deep_shot, "deep_throttle_s": round(deep[0] - t0, 1) if deep else None,
                       "cap4_mhz": [cap_max // 1000, cap_min // 1000], "cpu_peak_c": peak_c, "bins": bins}
    print(f"{d['label']:16s} policy={d.get('policy'):8s} onset shot={onset_shot} ({out[d['label']]['throttle_onset_s']} s) deep(<=75%) shot={deep_shot} ({out[d['label']]['deep_throttle_s']} s) cap4 {cap_max // 1000}->{cap_min // 1000} MHz peak {peak_c:.0f}C")
    for b in bins:
        print(f"   {b['shots']:>8s} proc {b['proc_ms_med']:>5.0f} ms  wall {b['wall_ms_med']:>5.0f} ms  N={b['frames_mean']}  gross {b['gross_j_per_shot']} J/shot")
json.dump(out, open(os.path.join(a.raw, "e3_detail.json"), "w"), indent=1)
