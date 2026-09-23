#!/usr/bin/env bash
# NDK 크로스빌드. 기본 arm64-v8a(Galaxy C55). 에뮬레이터 네이티브 실행용은 ABI=x86_64.
#   NDK=~/Android/Sdk/ndk/27.x bash scripts/build_arm64.sh
#   ABI=x86_64 bash scripts/build_arm64.sh   → build-x86_64/
# Windows(Git Bash)에서도 동작: NDK=$LOCALAPPDATA/Android/Sdk/ndk/27.0.12077973, cmake+ninja 필요
set -e
cd "$(dirname "$0")/.."
: "${NDK:?NDK 환경변수를 설정하세요 (예: ~/Android/Sdk/ndk/27.2.12479018)}"
#   NO_NEON=1 bash scripts/build_arm64.sh   → build-arm64-scalar/ (L0 scalar 측정용)
ABI=${ABI:-arm64-v8a}
OUT=build-${ABI%%-*}
EXTRA=()
if [ -n "$NO_NEON" ]; then OUT=$OUT-scalar; EXTRA=(-DBURSTPIPE_NO_NEON=ON); fi
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
cmake -S . -B "$OUT" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-30 "${EXTRA[@]}"
cmake --build "$OUT" -j
