#!/usr/bin/env bash
# core/src/shaders/*.comp → SPIR-V → C 헤더(*.spv.h). 생성물은 커밋한다 (PC 빌드에 glslc 불필요).
#   NDK=... bash scripts/build_shaders.sh
set -e
cd "$(dirname "$0")/.."
: "${NDK:?NDK 환경변수}"
GLSLC=$(ls "$NDK"/shader-tools/*/glslc* | head -1)
for f in core/src/shaders/*.comp; do
  n=$(basename "$f" .comp)
  "$GLSLC" -O --target-env=vulkan1.1 -mfmt=num -o "core/src/shaders/$n.spv.inc" "$f"
  { echo "// 생성: scripts/build_shaders.sh ← $n.comp (직접 수정 금지)"; echo "#pragma once"; echo "#include <cstdint>";
    echo "static const uint32_t k_${n}_spv[] = {"; cat "core/src/shaders/$n.spv.inc"; echo "};"; } > "core/src/shaders/$n.spv.h"
  rm "core/src/shaders/$n.spv.inc"
  echo "$f → core/src/shaders/$n.spv.h ($(grep -o '0x' core/src/shaders/$n.spv.h | wc -l) words)"
done
