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
unzip -o $T/tfl.aar 'headers/*' -d $T/h1 && cp -r $T/h1/headers/* third_party/litert/include/
unzip -o $T/gpu.aar 'headers/*' -d $T/h2 && cp -r $T/h2/headers/* third_party/litert/include/
[ -f models/selfie_segmenter.tflite ] || curl -L -o models/selfie_segmenter.tflite \
  https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter/float16/latest/selfie_segmenter.tflite
ls -la third_party/litert/lib/arm64-v8a models
test -f third_party/litert/include/tensorflow/lite/c/c_api.h && echo "headers OK" || echo "헤더 없음: 개발문서 Task 11 Step 1의 대체 방법 참고"
