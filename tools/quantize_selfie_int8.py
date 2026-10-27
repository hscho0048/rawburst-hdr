#!/usr/bin/env python3
"""사용: quantize_selfie_int8.py models/selfie_segmenter_std.tflite models/selfie_segmenter_int8.tflite bursts/device/* bursts/emu_portrait*
Hexagon HTP v68(Snapdragon 888 = Mi Mix 4)은 fp16 연산을 지원하지 않는다 (fp16 HTP는 v69부터) →
float16 모델을 QNN HTP 델리게이트에 넣으면 246개 연산이 모두 검증 실패(0xc26)하고 CPU로 떨어진다.
그래서 v68용으로 정적 INT8(가중치·활성 int8, 입출력은 float 유지) 모델을 만든다. 보정 입력은 실제 버스트의
frame 0을 앱과 같은 전처리(core/src/seg.cc bayer_to_rgb256: 2×2 슈퍼픽셀 → WB → ×ev 2.8 → 감마 → 정립 회전)로 만든다.
필요: pip install ai-edge-quantizer (ai-edge-litert 포함, TensorFlow 불필요). 검증: float vs int8 마스크 IoU.
"""
import glob
import os
import sys

import numpy as np
from ai_edge_litert.interpreter import Interpreter
from ai_edge_quantizer import quantizer, recipe

CFA = {0: [0, 1, 2, 3], 1: [1, 0, 3, 2], 2: [2, 3, 0, 1], 3: [3, 2, 1, 0]}  # 위치(dy*2+dx) → R Gr Gb B


def load_meta(d):
    m = {}
    for line in open(os.path.join(d, "meta.txt")):
        k, _, v = line.strip().partition(" ")
        if k != "frame":
            m[k] = v
    return m


def rgb256(d, ev=2.8, frame=None):
    m = load_meta(d)
    w, h = int(m["width"]), int(m["height"])
    raw = np.fromfile(frame or os.path.join(d, "frame_00.raw16"), dtype="<u2").reshape(h, w).astype(np.float32)
    bl = [float(x) for x in m.get("black_level", "64 64 64 64").split()]
    wl = float(m.get("white_level", 1023))
    g = [float(x) for x in m.get("wb_gains", "1 1 1 1").split()]
    cfa = CFA[int(m.get("cfa", 0))]
    sh, sw = h // 2, w // 2
    ys = np.minimum(sh - 1, ((np.arange(256) + 0.5) * sh / 256).astype(int))
    xs = np.minimum(sw - 1, ((np.arange(256) + 0.5) * sw / 256).astype(int))
    ch = np.zeros((4, 256, 256), np.float32)
    for dy in range(2):
        for dx in range(2):
            pos = dy * 2 + dx
            c = cfa[pos]
            v = raw[2 * ys[:, None] + dy, 2 * xs[None, :] + dx]
            ch[c] = np.clip((v - bl[pos]) * g[c] / (wl - bl[pos]), 0, 1)
    rgb = np.stack([ch[0], 0.5 * (ch[1] + ch[2]), ch[3]], -1)
    rgb = np.clip(rgb * ev, 0, 1) ** (1 / 2.2)
    k = (int(m.get("orientation", 0)) % 360) // 90
    return np.rot90(rgb, -k).copy()  # 시계방향 회전 = 정립


def add_signature(src, dst):
    """MediaPipe 모델에는 SignatureDef가 없다 → 양자화기가 요구하는 serving_default(입력 1, 출력 1)를 붙인다."""
    from ai_edge_litert import schema_py_generated as S
    from ai_edge_litert.tools import flatbuffer_utils as fu
    m = fu.read_model(src)
    sg = m.subgraphs[0]
    # float16 모델은 가중치·bias가 fp16 상수 + DEQUANTIZE(→fp32) 110개 → 양자화기가 bias를 상수로 못 읽는다.
    # DEQUANTIZE를 접어 fp32 상수로 만든다 (출력 텐서에 fp32 버퍼를 달고 op 제거). 수치는 fp16→fp32 확장이라 동일.
    deq = [i for i, oc in enumerate(m.operatorCodes) if max(oc.builtinCode, oc.deprecatedBuiltinCode) == S.BuiltinOperator.DEQUANTIZE]
    keep, folded = [], 0
    for op in sg.operators:
        if op.opcodeIndex in deq:
            ti, to = sg.tensors[op.inputs[0]], sg.tensors[op.outputs[0]]
            buf = m.buffers[ti.buffer].data if ti.buffer else None
            if ti.type == S.TensorType.FLOAT16 and buf is not None and len(buf):
                b = S.BufferT()
                b.data = np.frombuffer(bytes(bytearray(buf)), dtype=np.float16).astype(np.float32).tobytes()
                m.buffers.append(b)
                to.buffer = len(m.buffers) - 1
                folded += 1
                continue
        keep.append(op)
    sg.operators = keep
    print(f"folded {folded} fp16 DEQUANTIZE -> fp32 constants")
    def tmap(idx):
        t = S.TensorMapT()
        t.name = sg.tensors[idx].name
        t.tensorIndex = idx
        return t
    sd = S.SignatureDefT()
    sd.signatureKey = "serving_default"
    sd.subgraphIndex = 0
    sd.inputs = [tmap(i) for i in sg.inputs]
    sd.outputs = [tmap(o) for o in sg.outputs]
    if not m.signatureDefs:
        m.signatureDefs = [sd]
    fu.write_model(m, dst)


def run(model, x):
    it = Interpreter(model_path=model)
    it.allocate_tensors()
    i, o = it.get_input_details()[0], it.get_output_details()[0]
    x = x[None].astype(np.float32)
    if i["dtype"] in (np.int8, np.int16):  # 양자화 모델은 입출력도 정수 (seg_litert.cc가 같은 방식으로 변환)
        s, z = i["quantization"]
        lim = np.iinfo(i["dtype"])
        x = np.clip(np.round(x / s + z), lim.min, lim.max).astype(i["dtype"])
    it.set_tensor(i["index"], x)
    it.invoke()
    y = it.get_tensor(o["index"])[0, ..., 0]
    if o["dtype"] in (np.int8, np.int16):
        s, z = o["quantization"]
        y = (y.astype(np.float32) - z) * s
    return y


def main():
    src, dst, dirs = sys.argv[1], sys.argv[2], [d for a in sys.argv[3:] for d in glob.glob(a) if os.path.isfile(os.path.join(d, "meta.txt"))]
    base = [rgb256(d) for d in dirs]
    # 보정 세트: 실제 기기 버스트만, 버스트당 frame 0/4/7 + 좌우 반전.
    # 비교(실기기 7장면 평균 |Δα|): 이 방식 0.081 < 노출 ×0.5/×2 증강 0.138 < 에뮬 포함 단순 0.169.
    # 에뮬(합성) 인물과 노출 증강은 디코더 게이팅 경로(global avg pool → conv → sigmoid)의 활성 범위를 넓혀
    # per-tensor int8 해상도를 깎았다 (avg_pool_9: 실제 −2.7..1.5인데 범위 66.7 → 한 단계 0.26).
    calib = []
    for d in dirs:
        if "emu_" in os.path.basename(d):
            continue
        for fi in (0, 4, 7):
            fr = os.path.join(d, "frame_%02d.raw16" % fi)
            if os.path.exists(fr):
                x = rgb256(d, frame=fr)
                calib += [x, x[:, ::-1]]
    print(f"calibration samples: {len(calib)} (device bursts only)")
    sig_model = dst + ".sig.tflite"
    add_signature(src, sig_model)
    it = Interpreter(model_path=sig_model)
    key = list(it.get_signature_list().keys())[0]
    in_name = list(it.get_signature_runner(key).get_input_details().keys())[0]
    qt = quantizer.Quantizer(sig_model)
    qt.load_quantization_recipe(recipe.static_wi8_ai16() if os.environ.get("A16") else recipe.static_wi8_ai8())
    data = {key: [{in_name: c[None].astype(np.float32)} for c in calib]}
    res = qt.quantize(qt.calibrate(data))
    res.export_model(dst, overwrite=True)
    print("wrote", dst, os.path.getsize(dst), "bytes")
    # 검증: float vs int8 마스크 (0.5 이진화 IoU, 평균 |Δα|)
    for d, x in zip(dirs, base):
        a, b = run(src, x), run(dst, x)
        ia, ib = a > 0.5, b > 0.5
        iou = (ia & ib).sum() / max(1, (ia | ib).sum())
        print(f"{os.path.basename(d):20s} fg={ia.mean():.3f} IoU={iou:.4f} mean|d|={np.abs(a - b).mean():.4f}")


if __name__ == "__main__":
    main()
