#!/usr/bin/env bash
# (하드웨어 단계용) LiteRT(TFLite) C API arm64 .so + 헤더를 third_party/litert 에 받는다. 설계: 개발문서 Task 11 Step 1.
# 받은 뒤 build_arm64.sh / 앱 빌드를 다시 하면 BP_HAVE_LITERT가 켜지고 seg_litert.cc가 컴파일된다.
set -e
cd "$(dirname "$0")/.."
V=${LITERT_VERSION:-2.16.1}
T=$(mktemp -d)
mkdir -p third_party/litert/lib/arm64-v8a third_party/litert/include models
curl -L -o $T/tfl.aar  https://repo1.maven.org/maven2/org/tensorflow/tensorflow-lite/$V/tensorflow-lite-$V.aar
curl -L -o $T/gpu.aar  https://repo1.maven.org/maven2/org/tensorflow/tensorflow-lite-gpu/$V/tensorflow-lite-gpu-$V.aar
unzip -o -j $T/tfl.aar 'jni/arm64-v8a/libtensorflowlite_jni.so' -d third_party/litert/lib/arm64-v8a/
unzip -o -j $T/gpu.aar 'jni/arm64-v8a/libtensorflowlite_gpu_jni.so' -d third_party/litert/lib/arm64-v8a/
unzip -o -q $T/tfl.aar -d $T/h1 && cp -r $T/h1/headers/. third_party/litert/include/
unzip -o -q $T/gpu.aar -d $T/h2 && [ -d $T/h2/headers ] && cp -r $T/h2/headers/. third_party/litert/include/ || true
[ -f models/selfie_segmenter.tflite ] || curl -L -o models/selfie_segmenter.tflite \
  https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter/float16/latest/selfie_segmenter.tflite
# 커스텀 op → 표준 TRANSPOSE_CONV 변환 모델 (tensorflow 파이썬이 있으면). NPU 파티션 3 → 1
python3 tools/convert_selfie_model.py models/selfie_segmenter.tflite models/selfie_segmenter_std.tflite 2>/dev/null   || python tools/convert_selfie_model.py models/selfie_segmenter.tflite models/selfie_segmenter_std.tflite   || echo "변환 생략 (tensorflow 없음): 원본 모델 + 자체 커스텀 op 커널로도 동작"
ls -la third_party/litert/lib/arm64-v8a models
test -f third_party/litert/include/tensorflow/lite/c/c_api.h && echo "headers OK" || echo "헤더 없음: 개발문서 Task 11 Step 1의 대체 방법 참고"

# AAR의 headers/는 불완전하다 (예: tensorflow/lite/core/async/c/types.h 누락).
# seg_litert.cc를 헤더 검사만 컴파일(-fsyntax-only)하며 빠진 헤더를 같은 태그의 TF 저장소에서 받는다.
CXX="$NDK/toolchains/llvm/prebuilt/$(ls $NDK/toolchains/llvm/prebuilt | head -1)/bin/clang++"
for i in $(seq 1 40); do
  miss=$("$CXX" --target=aarch64-linux-android30 -std=c++17 -fsyntax-only -DBP_HAVE_LITERT=1 -Icore/include \
         -Ithird_party/litert/include core/src/seg_litert.cc 2>&1 | sed -n "s/.*fatal error: '\(tensorflow\/[^']*\)' file not found.*/\1/p" | head -1)
  [ -z "$miss" ] && { echo "seg_litert.cc 헤더 충족"; break; }
  echo "fetch $miss"
  mkdir -p "third_party/litert/include/$(dirname "$miss")"
  curl -sfL -o "third_party/litert/include/$miss" "https://raw.githubusercontent.com/tensorflow/tensorflow/v$V/$miss" || { echo "받기 실패: $miss"; break; }
done
