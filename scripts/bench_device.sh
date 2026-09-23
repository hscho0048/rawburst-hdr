#!/usr/bin/env bash
# 사용: bench_device.sh bursts/burst_xxx [cli 추가 인자...]   → build-arm64/t.json
#   BUILD=build-x86_64 로 에뮬레이터 네이티브 빌드 사용 (흐름 검증용, 시간 수치는 의미 없음)
#   ADB=/path/to/adb (기본: PATH의 adb)
set -e
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
BUILD=${BUILD:-build-arm64}
B=${1%/}; shift
D=/data/local/tmp/bp
NAME=$(basename "$B")
$ADB shell mkdir -p $D
$ADB push $BUILD/burstpipe_cli $D/ >/dev/null
$ADB shell chmod +x $D/burstpipe_cli
$ADB shell "test -f $D/$NAME/meta.txt" 2>/dev/null || $ADB push "$B" $D/ >/dev/null
[ -f "$B/mask_emu.bin" ] && $ADB push "$B/mask_emu.bin" $D/$NAME/ >/dev/null
ls third_party/litert/lib/arm64-v8a/*.so >/dev/null 2>&1 && $ADB push third_party/litert/lib/arm64-v8a/*.so $D/ >/dev/null
$ADB shell "cd $D && LD_LIBRARY_PATH=. ./burstpipe_cli --in $NAME --out out.ppm --json t.json $*"
$ADB pull $D/t.json $BUILD/t.json >/dev/null
