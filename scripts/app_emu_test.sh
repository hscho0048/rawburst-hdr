#!/usr/bin/env bash
# 앱을 에뮬레이터(또는 기기)에 설치하고 3가지 경로를 자동 실행해 logcat RESULT를 모은다.
#   1) emu  : 에뮬 인물 버스트(run-as로 앱 내부에 복사) → JNI → 에뮬 세그 → 보케 → JPEG
#   2) dump : 카메라 RAW 버스트 8장 → 디스크 덤프 + 처리  (덤프는 out/device_dump/ 로 회수 → PC cli 입력)
#   3) shoot: 카메라 RAW 버스트 → 처리 → JPEG, 셔터→JPEG 시간
# 사용: ADB=adb bash scripts/app_emu_test.sh [bursts/emu_portrait_half]
set -e
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
B=${1:-bursts/emu_portrait_half}; NAME=$(basename "$B")
PKG=dev.burstpipe
APK=android/app/build/outputs/apk/debug/app-debug.apk
$ADB install -r -g $APK > /dev/null
$ADB shell mkdir -p /data/local/tmp/bp
$ADB shell "test -f /data/local/tmp/bp/$NAME/meta.txt" 2>/dev/null || $ADB push "$B" /data/local/tmp/bp/ > /dev/null
$ADB shell "run-as $PKG sh -c 'rm -rf files/$NAME; mkdir -p files; cp -r /data/local/tmp/bp/$NAME files/'"
wait_result() {  # $1: 이전 RESULT 개수
  for i in $(seq 1 90); do
    n=$($ADB logcat -d -s burstpipe | grep -c "RESULT" || true)
    [ "$n" -gt "$1" ] && break; sleep 2
  done
  $ADB logcat -d -s burstpipe | grep -A6 "RESULT" | tail -7
}
$ADB logcat -c
$ADB shell am force-stop $PKG
$ADB shell am start -n $PKG/.MainActivity --es action emu > /dev/null
echo "== emu (JNI + 에뮬 세그 + 보케)"; wait_result 0
$ADB shell am start -n $PKG/.MainActivity --es action dump > /dev/null
echo "== dump (카메라 RAW 버스트)"; wait_result 1
$ADB shell am start -n $PKG/.MainActivity --es action shoot > /dev/null
echo "== shoot"; wait_result 2
# 덤프 회수
D=$($ADB shell "ls -d /sdcard/Android/data/$PKG/files/burst_* 2>/dev/null | tail -1" | tr -d '\r')
if [ -n "$D" ]; then
  rm -rf out/device_dump; mkdir -p out
  $ADB pull "$D" out/device_dump > /dev/null && echo "dump → out/device_dump ($(ls out/device_dump | wc -l) files)"
fi
$ADB shell "ls -t /sdcard/Android/data/$PKG/files/result_*.jpg | head -3" | tr -d '\r' | while read -r f; do $ADB pull "$f" out/ > /dev/null; done
ls out/result_*.jpg 2>/dev/null | tail -3
