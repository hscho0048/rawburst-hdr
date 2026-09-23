#!/usr/bin/env python3
"""사용: convert_selfie_model.py models/selfie_segmenter.tflite models/selfie_segmenter_std.tflite
MediaPipe 커스텀 op `Convolution2DTransposeBias`를 표준 TFLite `TRANSPOSE_CONV`(v3, bias 입력)로 바꾼 모델을 만든다.
→ 표준 LiteRT/파이썬 인터프리터에서 그대로 돌고, QNN HTP 델리게이트가 그래프 전체(파티션 1개)를 가져간다.
커스텀 옵션 = TfLiteTransposeConvParams{padding, stride_w, stride_h} (int32×3). 출력 shape은 모델에 이미 정적으로 있다.
필요: tensorflow (flatbuffer_utils, schema_py_generated)."""
import sys, struct
import numpy as np
from tensorflow.lite.python import schema_py_generated as S
from tensorflow.lite.tools import flatbuffer_utils as fu

src, dst = sys.argv[1], sys.argv[2]
m = fu.read_model(src)
BO = S.BuiltinOperator

def code_name(oc):
    return (oc.customCode.decode() if isinstance(oc.customCode, bytes) else oc.customCode) if oc.customCode else None

custom_idx = [i for i, oc in enumerate(m.operatorCodes) if code_name(oc) == "Convolution2DTransposeBias"]
if not custom_idx:
    print("커스텀 op 없음 — 변환 불필요"); fu.write_model(m, dst); sys.exit(0)

# TRANSPOSE_CONV 연산 코드 (v3: bias 입력 지원)
tc = S.OperatorCodeT()
tc.builtinCode = BO.TRANSPOSE_CONV
tc.deprecatedBuiltinCode = BO.TRANSPOSE_CONV
tc.version = 3
m.operatorCodes.append(tc)
tc_idx = len(m.operatorCodes) - 1

n = 0
for sg in m.subgraphs:
    for op in sg.operators:
        if op.opcodeIndex not in custom_idx:
            continue
        x, w, b = list(op.inputs)
        out = op.outputs[0]
        padding, stride_w, stride_h = struct.unpack("<3i", bytes(op.customOptions[:12]))
        shape = np.array(sg.tensors[out].shape, dtype=np.int32)
        # output_shape 상수 텐서
        buf = S.BufferT(); buf.data = shape.tobytes()
        m.buffers.append(buf)
        t = S.TensorT(); t.shape = [4]; t.type = S.TensorType.INT32; t.buffer = len(m.buffers) - 1
        t.name = b"tconv_output_shape_%d" % n
        sg.tensors.append(t)
        shape_t = len(sg.tensors) - 1
        opt = S.TransposeConvOptionsT()
        opt.padding = S.Padding.SAME if padding == 1 else S.Padding.VALID  # TfLitePadding: 1=Same 2=Valid
        opt.strideW, opt.strideH = stride_w, stride_h
        opt.fusedActivationFunction = S.ActivationFunctionType.NONE
        op.opcodeIndex = tc_idx
        op.inputs = [shape_t, w, x, b]
        op.builtinOptionsType = S.BuiltinOptions.TransposeConvOptions
        op.builtinOptions = opt
        op.customOptions = None
        op.customOptionsFormat = 0
        print(f"op → TRANSPOSE_CONV: x={sg.tensors[x].shape} w={sg.tensors[w].shape} out={list(shape)} "
              f"padding={'SAME' if padding == 1 else 'VALID'} stride=({stride_w},{stride_h})")
        n += 1
fu.write_model(m, dst)
print(f"{n}개 변환 → {dst}")
