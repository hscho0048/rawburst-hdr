#!/usr/bin/env bash
# 사용: perf_device.sh bursts/burst_xxx [cli 인자...]  → 핫스팟 상위 + 캐시 카운터 (먼저 bench_device.sh로 버스트 푸시)
set -e
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
: "${NDK:?}"
ADB=${ADB:-adb}
ARCH=${PERF_ARCH:-arm64}
D=/data/local/tmp/bp
$ADB push "$NDK/simpleperf/bin/android/$ARCH/simpleperf" $D/ >/dev/null
$ADB shell setprop security.perf_harden 0 || true
B=$(basename "${1%/}"); shift
$ADB shell "cd $D && LD_LIBRARY_PATH=. ./simpleperf record -e cpu-clock -f 4000 -g -o perf.data ./burstpipe_cli --in $B --out out.ppm $*"
$ADB shell "cd $D && ./simpleperf report -i perf.data --sort symbol -n | head -40"
echo "--- cache ---"
$ADB shell "cd $D && LD_LIBRARY_PATH=. ./simpleperf stat -e cpu-cycles,instructions,cache-references,cache-misses ./burstpipe_cli --in $B --out out.ppm $*"
