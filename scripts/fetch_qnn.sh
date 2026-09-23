#!/usr/bin/env bash
# (NPU 단계용) 로컬 QAIRT SDK에서 QNN TFLite 델리게이트 + HTP v69(SM7450) 라이브러리를 third_party/qnn 으로 복사.
#   QAIRT_SDK=~/Downloads/qairt/2.45.0.260326 bash scripts/fetch_qnn.sh   (Qualcomm 계정으로 받은 SDK. 재배포 불가 → git 제외)
set -e
cd "$(dirname "$0")/.."
: "${QAIRT_SDK:?QAIRT_SDK 환경변수 (QAIRT SDK 루트)}"
HTP=${HTP_ARCH:-v69}
mkdir -p third_party/qnn/include third_party/qnn/lib/arm64-v8a third_party/qnn/hexagon
cp -r "$QAIRT_SDK/include/QNN/TFLiteDelegate" third_party/qnn/include/
for f in libQnnTFLiteDelegate.so libQnnHtp.so libQnnHtpPrepare.so libQnnSystem.so "libQnnHtp${HTP^}Stub.so"; do
  cp "$QAIRT_SDK/lib/aarch64-android/$f" third_party/qnn/lib/arm64-v8a/
done
cp "$QAIRT_SDK/lib/hexagon-$HTP/unsigned/libQnnHtp${HTP^}Skel.so" third_party/qnn/hexagon/
# 앱 패키징용: skel도 jniLibs에 넣어 nativeLibraryDir에 풀리게 한다 (DSP 로더는 파일 경로가 필요)
cp third_party/qnn/hexagon/*.so third_party/qnn/lib/arm64-v8a/
ls -la third_party/qnn/lib/arm64-v8a third_party/qnn/hexagon
