#!/usr/bin/env python3
"""사용: seg_infer.py in.png out_mask.bin [models/selfie_segmenter.tflite]
실제 MediaPipe Selfie Segmentation(LiteRT)으로 256×256 float32 마스크를 만든다. (pip install ai-edge-litert numpy pillow)
에뮬레이터 인물 세트는 합성 인물이라 실제 모델이 잘 못 잡을 수 있다 → 그때는 bursts/emu_portrait/mask_emu.bin 을 쓴다."""
import sys, numpy as np
from PIL import Image
from ai_edge_litert.interpreter import Interpreter

src, dst = sys.argv[1], sys.argv[2]
model = sys.argv[3] if len(sys.argv) > 3 else "models/selfie_segmenter.tflite"
it = Interpreter(model_path=model, num_threads=4); it.allocate_tensors()
inp, out = it.get_input_details()[0], it.get_output_details()[0]
_, H, W, _ = inp["shape"]
img = Image.open(src).convert("RGB").resize((W, H), Image.BILINEAR)
x = (np.asarray(img, np.float32) / 255.0)[None]
it.set_tensor(inp["index"], x); it.invoke()
y = it.get_tensor(out["index"])[0]           # (H,W,1) 또는 (H,W,2)
m = y[..., -1] if y.shape[-1] == 2 else y[..., 0]
if m.min() < -0.01 or m.max() > 1.01:         # 로짓이면 시그모이드
    m = 1 / (1 + np.exp(-m))
print("input", inp["shape"], "output", y.shape, "mask range", float(m.min()), float(m.max()), "fg ratio", float((m > 0.5).mean()))
m.astype(np.float32).tofile(dst)
Image.fromarray((m * 255).astype(np.uint8)).save(dst + ".png")
