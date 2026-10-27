#!/usr/bin/env python3
"""results/power/<dev>/*.json → docs/power.md용 마크다운 표 (기기 전체·배터리 기준).
  python tools/power_report.py c55 mix4
실험별로 가장 최근 런 파일(E1_*.json, E2_*.json, ...)을 쓴다. 숫자는 반복(r1..rN) 중앙값, 괄호는 최소–최대."""
import glob
import json
import os
import re
import statistics as st
import sys


def latest(dev, exp):
    fs = sorted(glob.glob(f"results/power/{dev}/{exp}_*.json"), key=os.path.getmtime)
    return json.load(open(fs[-1], encoding="utf-8")) if fs else None


def rng(ws, k, f=1.0, nd=1):
    v = [w[k] * f for w in ws if w.get(k) is not None]
    if not v:
        return "—"
    m = st.median(v)
    return f"{m:.{nd}f} ({min(v):.{nd}f}–{max(v):.{nd}f})" if len(v) > 1 else f"{m:.{nd}f}"


def groups(d):
    g = {}
    for w in d["windows"]:
        if "error" in w:
            continue
        g.setdefault(re.sub(r"_r\d+_", "_", w["label"]), []).append(w)
    return g


def clock(ws, cpu):
    v = [w["clocks"][cpu]["mean_mhz"] for w in ws if w.get("clocks") and cpu in w["clocks"]]
    cap = [w["clocks"][cpu]["cap_min_mhz"] for w in ws if w.get("clocks") and cpu in w["clocks"]]
    return f"{st.median(v):.0f}/{min(cap):.0f}" if v else "—"


def temps(ws):
    a = [w["cpu_max_c"][0] for w in ws if w.get("cpu_max_c")]
    b = [w["cpu_max_c"][1] for w in ws if w.get("cpu_max_c")]
    return f"{st.median(a):.0f}→{st.median(b):.0f}" if a else "—"


def table_e1(dev):
    d = latest(dev, "E1")
    if not d:
        return f"(E1 {dev}: 없음)\n"
    s = f"**E1 세그 백엔드 — {dev} (기기 전체(배터리) 기준, 추론 1회당, {d['windows'][0]['seconds']:.0f}초+ 반복 구간)**\n\n"
    s += "| 백엔드 | net mJ/회 | net W | 평균 W (gross) | idle W | ms/회 | CPU °C 시작→끝 | 반복 |\n|---|---|---|---|---|---|---|---|\n"
    for k, ws in groups(d).items():
        s += f"| {k.split('_', 1)[1]} | {rng(ws, 'net_j_per_iter', 1e3, 2)} | {rng(ws, 'net_w', 1, 2)} | {rng(ws, 'avg_w', 1, 2)} | {rng(ws, 'idle_w', 1, 2)} | {rng(ws, 'ms_per_iter', 1, 2)} | {temps(ws)} | {len(ws)} |\n"
    return s


def table_e2(dev):
    d = latest(dev, "E2")
    if not d:
        return f"(E2 {dev}: 없음)\n"
    s = f"**E2 합성(align+merge+finish) 코어 배치 — {dev} (기기 전체(배터리) 기준, burst 1회 = 4080×3072×8)**\n\n"
    s += "| 배치 | net J/burst | ms/burst | net W | 평균 W | idle W | cpu4 / cpu7 평균·상한 MHz | CPU °C | 반복 |\n|---|---|---|---|---|---|---|---|---|\n"
    for k, ws in groups(d).items():
        s += (f"| {k.split('_', 1)[1]} | {rng(ws, 'net_j_per_iter', 1, 3)} | {rng(ws, 'ms_per_iter', 1, 0)} | {rng(ws, 'net_w', 1, 2)} | "
              f"{rng(ws, 'avg_w', 1, 2)} | {rng(ws, 'idle_w', 1, 2)} | {clock(ws, 'cpu4')} · {clock(ws, 'cpu7')} | {temps(ws)} | {len(ws)} |\n")
    return s


def table_e4(dev):
    d = latest(dev, "E4")
    if not d:
        return f"(E4 {dev}: 없음)\n"
    s = f"**E4 앱 단계별 에너지 — {dev} (기기 전체(배터리) 기준, 카메라 프리뷰 켠 상태 대비 순 에너지, 단계당 30초 반복)**\n\n"
    s += "| 단계 | net J/회 | ms/회 | net W | 평균 W | idle(프리뷰) W | 반복 |\n|---|---|---|---|---|---|---|\n"
    for k, ws in groups(d).items():
        s += f"| {k.split('_', 1)[1]} | {rng(ws, 'net_j_per_iter', 1, 3)} | {rng(ws, 'ms_per_iter', 1, 0)} | {rng(ws, 'net_w', 1, 2)} | {rng(ws, 'avg_w', 1, 2)} | {rng(ws, 'idle_w', 1, 2)} | {len(ws)} |\n"
    return s


def table_e3(dev):
    d = latest(dev, "E3")
    if not d:
        return f"(E3 {dev}: 없음)\n"
    s = f"**E3 150연속 촬영 — {dev} (기기 전체(배터리) 기준, 셔터→처리→JPEG, 세그·보케 끔)**\n\n"
    s += "| 정책 | 총 net J | net J/장 | 셔터→JPEG ms/장 | 구간 s | 평균 W | idle(프리뷰) W | 반복 |\n|---|---|---|---|---|---|---|---|\n"
    for k, ws in groups(d).items():
        s += (f"| {k.split('_', 1)[1]} | {rng(ws, 'net_j', 1, 0)} | {rng(ws, 'net_j_per_iter', 1, 2)} | {rng(ws, 'iter_ms_median', 1, 0)} | "
              f"{rng(ws, 'seconds', 1, 0)} | {rng(ws, 'avg_w', 1, 2)} | {rng(ws, 'idle_w', 1, 2)} | {len(ws)} |\n")
    return s


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    for dev in sys.argv[1:] or ["c55", "mix4"]:
        for f in (table_e1, table_e2, table_e3, table_e4):
            print(f(dev))
