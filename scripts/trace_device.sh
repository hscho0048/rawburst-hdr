#!/usr/bin/env bash
# 사용: trace_device.sh bursts/burst_xxx [cli 인자...]  → build-arm64/trace.atr (ui.perfetto.dev에서 연다)
set -e
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
BUILD=${BUILD:-build-arm64}
$ADB shell "atrace --async_start -a '*' -b 32768 sched freq idle"
bash scripts/bench_device.sh "$@"
$ADB shell "atrace --async_stop -z -o /data/local/tmp/trace.atr"
$ADB pull /data/local/tmp/trace.atr $BUILD/trace.atr >/dev/null
echo "open https://ui.perfetto.dev → Open trace file → $BUILD/trace.atr"
