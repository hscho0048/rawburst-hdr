#!/usr/bin/env bash
# 앱 전력 실험 (E3 150연속 촬영 거버너 켬/끔, E4 단계별 에너지). 케이블 분리 + 무선 adb.
#   ADB="adb -s <phone-ip>:5555" bash scripts/power_app.sh c55 E4 3      → results/power/c55/E4_<run>.json
#   ADB=... DELEGATE=gpu bash scripts/power_app.sh mix4 E3 3
# 앱은 화면 켜짐·밝기 고정(창 밝기 BRIGHT, 기본 0.4)·카메라 프리뷰 켜짐. idle 기준선 = 같은 상태로 프리뷰만 30초.
# 기기 쪽 러너가 am start로 단계를 넘기고 결과 JSON 파일이 생길 때까지 기다린다 (구간 중 adb/Wi-Fi 트래픽 없음).
set -e
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
DEV=${1:?device}; EXP=${2:?E3|E4}; ROUNDS=${3:-3}
DELEGATE=${DELEGATE:-npu}; BRIGHT=${BRIGHT:-0.4}; SECS=${SECS:-30}; COUNT=${COUNT:-150}
PKG=dev.burstpipe; D=/data/local/tmp/bp
RUN=${RUN:-${EXP}_$(date +%m%d_%H%M)}
OUT=results/power/$DEV; RAW=$OUT/raw/$RUN
F=/sdcard/Android/data/$PKG/files/power   # 앱이 쓰는 기록 위치

case $EXP in
  E4) STAGES=${STAGES:-"capture plain portrait seg jpeg"} ;;
  E3) STAGES=${STAGES:-"off thermal"} ;;
  *) echo "E3|E4"; exit 2 ;;
esac

mkdir -p out; R=out/power_run_$RUN.sh
{
  echo "D=$D; RUN=$RUN; P=\$D/power/\$RUN; F=$F; PKG=$PKG; mkdir -p \$P"
  echo "A=\"am start -n \$PKG/.MainActivity --es delegate $DELEGATE --ef brightness $BRIGHT\""
  cat <<'EOS'
cpu_t() { m=0; for z in /sys/class/thermal/thermal_zone*; do case "$(cat $z/type 2>/dev/null)" in *cpu*) v=$(cat $z/temp 2>/dev/null); [ "$v" -gt "$m" ] 2>/dev/null && [ "$v" -lt 150000 ] && m=$v;; esac; done; echo $m; }
log() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $P/status.log; }
cool() { s=0; while [ $(cpu_t) -gt $((T0 + 1000)) ] && [ $s -lt 900 ]; do sleep 5; s=$((s + 5)); done; log "cool waited=${s}s cpu=$(cpu_t) T0=$T0"; }
waitf() { s=0; while [ ! -f $F/$1.json ] && [ $s -lt $2 ]; do sleep 2; s=$((s + 2)); done; [ -f $F/$1.json ] && log "ok $1" || log "TIMEOUT $1"; }
# 1초 주기 클럭·온도 모니터 (스로틀 진입 시점용): uptime cpu0 cpu4 cpu7 cap4 cap7 cpu_max_mC
( while [ ! -f $P/DONE ]; do c=/sys/devices/system/cpu; echo "$(cut -d' ' -f1 /proc/uptime) $(cat $c/cpu0/cpufreq/scaling_cur_freq) $(cat $c/cpu4/cpufreq/scaling_cur_freq) $(cat $c/cpu7/cpufreq/scaling_cur_freq) $(cat $c/cpu4/cpufreq/scaling_max_freq) $(cat $c/cpu7/cpufreq/scaling_max_freq) $(cpu_t)"; sleep 1; done > $P/monitor.txt ) &
# MIUI 스와이프 잠금: 깨운 뒤 keyguard가 떠 있으면 activityTimeoutWM=10 s로 곧 꺼진다 → 스와이프와 같은 dismiss
input keyevent 224; sleep 1; wm dismiss-keyguard; settings put system screen_brightness_mode 0
am force-stop $PKG; sleep 1
# 샘플러는 force-stop 뒤에 시작 (force-stop은 같은 패키지의 서비스도 죽인다)
am start-foreground-service -n $PKG/.PowerService --es name $RUN --ei period_ms 50 >/dev/null; sleep 2
$A; sleep 15   # 카메라·세그 델리게이트 init (GPU OpenCL init 2-3 s)
# 사전 안정화: 프리뷰 켠 상태에서 CPU 온도가 1분에 0.5°C 넘게 변하지 않을 때까지 (최대 15분) → T0
pre() { a=$(cpu_t); s=0; while [ $s -lt 900 ]; do sleep 60; s=$((s + 60)); b=$(cpu_t); d=$((a - b)); [ ${d#-} -lt 500 ] && break; a=$b; done; log "precool ${s}s cpu=$(cpu_t)"; }
pre
T0=$(cpu_t); log "start T0=$T0 level=$(dumpsys battery | grep -m1 ' level')"
EOS
  for r in $(seq 1 "$ROUNDS"); do
    # 순서 번갈아: 홀수 회차 정방향, 짝수 회차 역방향
    S=$STAGES; [ $((r % 2)) -eq 0 ] && S=$(echo $STAGES | tr ' ' '\n' | tac | tr '\n' ' ')
    for st in $S; do
      L="${EXP}_r${r}_${st}"
      echo "cool"
      echo "\$A --es action pstage --es stage idle --ei seconds $SECS --es label ${L}_idle >/dev/null; waitf ${L}_idle 120"
      if [ "$EXP" = E4 ]; then
        echo "\$A --es action pstage --es stage $st --ei seconds $SECS --es label $L >/dev/null; waitf $L 300"
      else
        echo "\$A --es action stress --ei count $COUNT --es policy $st --ez noseg true --es label $L >/dev/null; waitf $L 1200"
      fi
    done
  done
  echo 'log "end level=$(dumpsys battery | grep -m1 " level")"; am start-foreground-service -n $PKG/.PowerService --es cmd stop >/dev/null; s=0; while [ ! -f $F/$RUN.csv.done ] && [ $s -lt 30 ]; do sleep 1; s=$((s + 1)); done; am force-stop $PKG; input keyevent 223; echo done > $P/DONE'
} > "$R"
$ADB shell mkdir -p $D/power
$ADB push "$R" $D/power/run_$RUN.sh >/dev/null; rm -f "$R"

$ADB shell "dumpsys deviceidle whitelist +$PKG >/dev/null; cmd appops set $PKG RUN_ANY_IN_BACKGROUND allow; cmd appops set $PKG RUN_IN_BACKGROUND allow; pm grant $PKG android.permission.CAMERA"
$ADB shell "nohup sh $D/power/run_$RUN.sh > $D/power/run_$RUN.log 2>&1 &"
echo "[$DEV $EXP] run=$RUN rounds=$ROUNDS stages=$STAGES delegate=$DELEGATE started"
while ! $ADB shell "test -f $D/power/$RUN/DONE" 2>/dev/null; do
  sleep ${POLL:-60}
  $ADB shell "tail -1 $D/power/$RUN/status.log" 2>/dev/null | tr -d '\r' || echo "(adb 연결 끊김?)"
done
mkdir -p "$RAW"
$ADB pull $D/power/$RUN/. "$RAW/" >/dev/null
$ADB pull $F/$RUN.csv "$RAW/samples.csv" >/dev/null
for f in $($ADB shell "ls $F/${EXP}_r*.json" | tr -d '\r'); do $ADB pull "$f" "$RAW/" >/dev/null; done
$ADB shell "rm -f $F/${EXP}_r*.json"
python tools/power_analyze.py analyze --csv "$RAW/samples.csv" --records "$RAW" --calib "$OUT/calibration.json" --out "$OUT/$RUN.json"
