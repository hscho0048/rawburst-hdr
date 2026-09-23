#!/usr/bin/env bash
# 사용: perf_device.sh bursts/burst_xxx [cli 인자...]  → 핫스팟 상위 + 캐시 카운터 (먼저 bench_device.sh로 버스트 푸시)
# user 빌드(양산폰)는 커널 샘플 금지 → 모든 이벤트에 :u. cpu-clock/task-clock 소프트웨어 이벤트는 삼성 user 빌드에서 막혀 있다.
set -e
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
: "${NDK:?}"
ADB=${ADB:-adb}
ARCH=${PERF_ARCH:-arm64}
D=/data/local/tmp/bp
$ADB push "$NDK/simpleperf/bin/android/$ARCH/simpleperf" $D/ >/dev/null
$ADB shell chmod +x $D/simpleperf
$ADB shell setprop security.perf_harden 0 || true
B=$(basename "${1%/}"); shift
$ADB shell "cd $D && LD_LIBRARY_PATH=. ./simpleperf record -e ${PERF_EVENT:-cpu-cycles:u} -f 4000 -g -o perf.data ./burstpipe_cli --in $B --out out.ppm $*"
$ADB shell "cd $D && ./simpleperf report -i perf.data --sort symbol -n | head -40"
echo "--- cache ---"
$ADB shell "cd $D && LD_LIBRARY_PATH=. ./simpleperf stat -e cpu-cycles:u,instructions:u,cache-references:u,cache-misses:u,L1-dcache-load-misses:u,LLC-load-misses:u,stalled-cycles-backend:u ./burstpipe_cli --in $B --out out.ppm $*"
