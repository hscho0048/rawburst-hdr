#!/usr/bin/env python3
"""전력 샘플(PowerService CSV) × 구간 기록(cli/앱 JSON) → 구간별 에너지 (기기 전체·배터리 기준).

  python tools/power_analyze.py validate --csv val.csv --records DIR [--dumpsys val_dumpsys.txt]
      단위(µA/mA)·부호·연료게이지 갱신 주기 추정 + 화면 밝기 변화로 방향 확인 → calibration.json
  python tools/power_analyze.py analyze --csv run.csv [--csv run2.csv ...] --records DIR --calib calibration.json
      --out results/power/<dev>/<exp>.json [--prefix E2_]

구간 이름 규칙: <exp>_r<round>_<cfg> (작업) + <exp>_r<round>_<cfg>_idle (바로 앞 idle 기준선, 같은 조건 30초)
net_J = gross_J − idle_W × 작업 구간 길이.  주 지표: net J/회 (반복 중앙값).
"""
import argparse
import csv
import glob
import json
import os
import re
import statistics as st
import sys


def load_csv(paths):
    rows = []
    for p in paths:
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                try:
                    rows.append((int(r["t_ns"]), int(r["current_raw"]), int(r["charge_uah"]), int(r["voltage_mv"]),
                                 int(r["temp_dc"]), int(r["plugged"])))
                except (ValueError, KeyError):
                    pass
    rows.sort()
    return rows


def load_records(src):
    files = []
    for s in src:
        files += sorted(glob.glob(os.path.join(s, "*.json"))) if os.path.isdir(s) else [s]
    recs = {}
    for p in files:
        try:
            with open(p) as f:
                r = json.load(f)
        except (json.JSONDecodeError, OSError):
            continue
        if "t0_ns" in r and "label" in r:
            recs[r["label"]] = r
    return recs


def window(rows, t0, t1):
    return [r for r in rows if t0 <= r[0] <= t1]


def update_period_ms(rows):
    """연료게이지 실제 갱신 주기: current 값이 바뀌는 시점 간격의 중앙값 (같은 값 반복 = 갱신 안 됨)."""
    ch = [rows[i][0] for i in range(1, len(rows)) if rows[i][1] != rows[i - 1][1]]
    d = [(ch[i] - ch[i - 1]) / 1e6 for i in range(1, len(ch))]
    if not d:
        return None, None
    return st.median(d), (sorted(d)[len(d) // 10], sorted(d)[len(d) * 9 // 10])


def integrate(rows, t0, t1, calib):
    """[t0,t1] 에너지 J (방전 양수). 샘플은 t0 앞뒤 포함해 구간 경계에서 잘라 사다리꼴 적분."""
    scale = 1e-6 if calib["current_unit"] == "uA" else 1e-3
    sign = calib["discharge_sign"]
    w = [r for r in rows if t0 - 2e8 <= r[0] <= t1 + 2e8]
    if len(w) < 3:
        return None
    def p(r):
        return sign * r[1] * scale * r[3] * 1e-3  # W
    e = 0.0
    for a, b in zip(w, w[1:]):
        lo, hi = max(a[0], t0), min(b[0], t1)
        if hi <= lo:
            continue
        e += 0.5 * (p(a) + p(b)) * (hi - lo) * 1e-9
    inside = window(rows, t0, t1)
    plugged = any(r[5] != 0 for r in inside)
    cc = None
    if len(inside) >= 2 and inside[0][2] > 0:
        # 쿨롱 카운터 교차 확인: Δcharge(µAh) × 평균 전압 → J (해상도 거침, 참고값)
        dq = (inside[0][2] - inside[-1][2]) * 1e-6 * 3600.0
        cc = dq * st.mean(r[3] for r in inside) * 1e-3
    return {"gross_j": e, "n_samples": len(inside), "plugged": plugged, "cc_j": cc,
            "batt_temp_c": [inside[0][4] / 10.0, inside[-1][4] / 10.0] if inside else None,
            "voltage_mv": [inside[0][3], inside[-1][3]] if inside else None}


def cmd_validate(a):
    rows = load_csv(a.csv)
    recs = load_records(a.records)
    cur = [r[1] for r in rows]
    med_abs = st.median(abs(c) for c in cur)
    unit = "uA" if med_abs > 20000 else "mA"
    # 케이블 분리 + 화면 꺼짐 idle은 반드시 방전 → 그 구간 중앙값 부호가 방전 부호
    off = recs.get("val_screen_off")
    ref = window(rows, off["t0_ns"], off["t1_ns"]) if off else rows
    med_off = st.median(r[1] for r in ref)
    sign = -1 if med_off < 0 else 1
    period, p10_90 = update_period_ms(rows)
    dt = [(rows[i][0] - rows[i - 1][0]) / 1e6 for i in range(1, len(rows))]
    calib = {"current_unit": unit, "discharge_sign": sign, "median_abs_current_raw": med_abs,
             "screen_off_median_raw": med_off, "gauge_update_ms_median": period, "gauge_update_ms_p10_p90": p10_90,
             "sample_period_ms_median": st.median(dt) if dt else None, "n_samples": len(rows),
             "plugged_any": any(r[5] != 0 for r in rows), "windows": {}}
    for k in ("val_screen_off", "val_bright_min", "val_bright_max"):
        r = recs.get(k)
        if not r:
            continue
        e = integrate(rows, r["t0_ns"], r["t1_ns"], calib)
        sec = (r["t1_ns"] - r["t0_ns"]) / 1e9
        calib["windows"][k] = {"seconds": sec, "avg_w": e["gross_j"] / sec if e else None,
                               "median_raw": st.median(x[1] for x in window(rows, r["t0_ns"], r["t1_ns"])),
                               "cc_avg_w": (e["cc_j"] / sec) if e and e["cc_j"] is not None else None,
                               "plugged": e["plugged"] if e else None}
    w = calib["windows"]
    if "val_bright_min" in w and "val_bright_max" in w:
        calib["brightness_check"] = {
            "max_minus_min_w": w["val_bright_max"]["avg_w"] - w["val_bright_min"]["avg_w"],
            "ok": w["val_bright_max"]["avg_w"] > w["val_bright_min"]["avg_w"] > w.get("val_screen_off", {}).get("avg_w", 0)}
    if a.dumpsys and os.path.exists(a.dumpsys):
        vals = [int(m.group(1)) for m in (re.search(r"currentnow:(-?\d+)", l) for l in open(a.dumpsys)) if m]
        if vals:
            calib["dumpsys_current_now_median"] = st.median(vals)
    print(json.dumps(calib, indent=1))
    if a.out:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        json.dump(calib, open(a.out, "w"), indent=1)


def cmd_analyze(a):
    rows = load_csv(a.csv)
    recs = load_records(a.records)
    calib = json.load(open(a.calib))
    out = {"calibration": {k: calib[k] for k in ("current_unit", "discharge_sign", "gauge_update_ms_median")},
           "basis": "기기 전체(배터리) 기준 — SoC 단독 전력 아님", "windows": [], "summary": {}}
    for label, r in sorted(recs.items(), key=lambda kv: kv[1]["t0_ns"]):
        if a.prefix and not label.startswith(a.prefix):
            continue
        if label.endswith("_idle"):
            continue
        idle = recs.get(label + "_idle") or (recs.get(r["idle_label"]) if r.get("idle_label") else None)
        e = integrate(rows, r["t0_ns"], r["t1_ns"], calib)
        if e is None:
            out["windows"].append({"label": label, "error": "no samples"})
            continue
        sec = (r["t1_ns"] - r["t0_ns"]) / 1e9
        w = {"label": label, "mode": r.get("mode"), "seconds": round(sec, 3), "iters": r.get("iters", 0),
             "gross_j": round(e["gross_j"], 4), "avg_w": round(e["gross_j"] / sec, 4),
             "ms_per_iter": r.get("ms_per_iter"), "iter_ms_median": r.get("iter_ms_median"),
             "cpu_max_c": r.get("cpu_max_c"), "skin_c": r.get("skin_c"), "batt_temp_c": e["batt_temp_c"],
             "clocks": r.get("clocks"), "plugged": e["plugged"], "n_samples": e["n_samples"], "voltage_mv": e["voltage_mv"],
             "cc_gross_j": round(e["cc_j"], 3) if e["cc_j"] is not None else None, "args": r.get("args")}
        if idle:
            ei = integrate(rows, idle["t0_ns"], idle["t1_ns"], calib)
            if ei:
                isec = (idle["t1_ns"] - idle["t0_ns"]) / 1e9
                w["idle_w"] = round(ei["gross_j"] / isec, 4)
                w["net_j"] = round(e["gross_j"] - w["idle_w"] * sec, 4)
                w["net_w"] = round(w["net_j"] / sec, 4)
                if w["iters"]:
                    w["net_j_per_iter"] = round(w["net_j"] / w["iters"], 5)
                    w["gross_j_per_iter"] = round(e["gross_j"] / w["iters"], 5)
                w["idle_plugged"] = ei["plugged"]
        out["windows"].append(w)
    # 반복(r1, r2, ...)을 묶어 중앙값
    groups = {}
    for w in out["windows"]:
        cfg = re.sub(r"_r\d+_", "_", w["label"])
        groups.setdefault(cfg, []).append(w)
    for cfg, ws in groups.items():
        def med(k):
            v = [x[k] for x in ws if x.get(k) is not None]
            return round(st.median(v), 5) if v else None
        out["summary"][cfg] = {"n": len(ws), "net_j_per_iter": med("net_j_per_iter"), "net_w": med("net_w"),
                               "avg_w": med("avg_w"), "idle_w": med("idle_w"), "ms_per_iter": med("ms_per_iter"),
                               "iter_ms_median": med("iter_ms_median"), "iters": med("iters"),
                               "net_j_per_iter_all": [x.get("net_j_per_iter") for x in ws],
                               "any_plugged": any(x.get("plugged") or x.get("idle_plugged") for x in ws)}
    s = json.dumps(out, indent=1, ensure_ascii=False)
    if a.out:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        open(a.out, "w", encoding="utf-8").write(s)
    for cfg, v in out["summary"].items():
        print(f"{cfg:40s} n={v['n']} netJ/it={v['net_j_per_iter']} netW={v['net_w']} avgW={v['avg_w']} idleW={v['idle_w']} ms/it={v['ms_per_iter']}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("validate")
    v.add_argument("--csv", nargs="+", required=True)
    v.add_argument("--records", nargs="+", required=True)
    v.add_argument("--dumpsys")
    v.add_argument("--out")
    z = sub.add_parser("analyze")
    z.add_argument("--csv", nargs="+", required=True)
    z.add_argument("--records", nargs="+", required=True)
    z.add_argument("--calib", required=True)
    z.add_argument("--out")
    z.add_argument("--prefix")
    a = ap.parse_args()
    {"validate": cmd_validate, "analyze": cmd_analyze}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
