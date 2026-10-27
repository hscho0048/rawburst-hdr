#!/usr/bin/env bash
# cli 전력 실험 (E1 세그 백엔드 / E2 코어 배치). 케이블을 뺀 상태에서 무선 adb로 실행한다 (docs/power.md 2장).
#   ADB="adb -s <phone-ip>:5555" bash scripts/power_cli.sh c55 E2 3      → results/power/c55/E2.json
#   ADB=... BURST=test0 bash scripts/power_cli.sh mix4 E1 3
# 흐름: PowerService(앱) 샘플링 시작 → 기기에서 nohup 러너 (화면 끔 → [쿨다운 → idle 30s → 작업 ≥30s] × 설정 × 반복,
#       순서는 번갈아 A B C A B C) → 끝나면 CSV·구간 JSON 회수 → tools/power_analyze.py
# 기기 쪽에서 돌리는 이유: 구간 중 adb 트래픽(Wi-Fi)이 측정에 섞이지 않게. PC는 1분마다 상태 파일만 본다.
set -e
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
ADB=${ADB:-adb}
DEV=${1:?device name (c55|mix4)}; EXP=${2:?E1|E2}; ROUNDS=${3:-3}
SECONDS_WORK=${SECONDS_WORK:-30}; SECONDS_IDLE=${SECONDS_IDLE:-30}
BURST=${BURST:-test0}
# SAMPLER=app(기본, 앱 PowerService) | shell(app_process, shell uid — MIUI AutoPowerKill 대상 아님. Mix 4는 이것)
SAMPLER=${SAMPLER:-app}
MODEL=${MODEL:-selfie_segmenter_std.tflite}
D=/data/local/tmp/bp
RUN=${RUN:-${EXP}_$(date +%m%d_%H%M)}
OUT=results/power/$DEV
RAW=$OUT/raw/$RUN

# 설정: "이름|바이너리|인자"
case "$EXP:$DEV" in
  E1:*) CFGS=("npu|burstpipe_cli|--seg-only --seg-model $MODEL --delegate npu"
              "gpu|burstpipe_cli|--seg-only --seg-model $MODEL --delegate gpu"
              "cpu|burstpipe_cli|--seg-only --seg-model $MODEL --delegate cpu") ;;
  E2:c55) CFGS=("scalar_prime1|burstpipe_cli_scalar|--threads 1 --cpus 7"
                "neon_little4|burstpipe_cli|--threads 4 --cpus 0,1,2,3"
                "neon_big4|burstpipe_cli|--threads 4 --cpus 4,5,6,7"
                "neon_prime1|burstpipe_cli|--threads 1 --cpus 7"
                "neon_unpinned8|burstpipe_cli|--threads 8") ;;
  E2:mix4) CFGS=("scalar_prime1|burstpipe_cli_scalar|--threads 1 --cpus 7"
                 "neon_little4|burstpipe_cli|--threads 4 --cpus 0,1,2,3"
                 "neon_mid3|burstpipe_cli|--threads 3 --cpus 4,5,6"
                 "neon_prime1|burstpipe_cli|--threads 1 --cpus 7"
                 "neon_midprime4|burstpipe_cli|--threads 4 --cpus 4,5,6,7"
                 "neon_unpinned8|burstpipe_cli|--threads 8") ;;
  *) echo "unknown $EXP:$DEV"; exit 2 ;;
esac
[ -n "$ONLY" ] && { F=(); for c in "${CFGS[@]}"; do [[ ",$ONLY," == *",${c%%|*},"* ]] && F+=("$c"); done; CFGS=("${F[@]}"); }

# 바이너리·라이브러리·입력 (없을 때만)
$ADB shell mkdir -p $D/power
$ADB push build-arm64/burstpipe_cli $D/ >/dev/null
$ADB push build-arm64-scalar/burstpipe_cli $D/burstpipe_cli_scalar >/dev/null
$ADB shell chmod +x $D/burstpipe_cli $D/burstpipe_cli_scalar
$ADB shell "test -f $D/$BURST/meta.txt" 2>/dev/null || $ADB push "bursts/device/$BURST" $D/ >/dev/null
$ADB shell "test -f $D/libtensorflowlite_jni.so" || $ADB push third_party/litert/lib/arm64-v8a/. $D/ >/dev/null
$ADB shell "test -f $D/libQnnHtp.so" || { $ADB push third_party/qnn/lib/arm64-v8a/. $D/ >/dev/null; $ADB push third_party/qnn/hexagon/. $D/ >/dev/null; }
for so in third_party/qnn/lib/arm64-v8a/libQnnHtpV*; do  # v68/v69 둘 다 (기기 HTP 버전은 QNN이 런타임 감지)
  $ADB shell "test -f $D/$(basename $so)" || $ADB push "$so" $D/ >/dev/null
done
$ADB shell "test -f $D/$MODEL" || $ADB push "models/$MODEL" $D/ >/dev/null

# 기기 러너 생성
mkdir -p out; R=out/power_run_$RUN.sh
{
  echo "D=$D; RUN=$RUN; P=\$D/power/\$RUN; mkdir -p \$P; cd \$D"
  echo "export LD_LIBRARY_PATH=\$D ADSP_LIBRARY_PATH=\"\$D;/vendor/dsp/cdsp;/vendor/lib/rfsa/adsp;/system/lib/rfsa/adsp;/dsp\""
  cat <<'EOS'
cpu_t() { m=0; for z in /sys/class/thermal/thermal_zone*; do case "$(cat $z/type 2>/dev/null)" in *cpu*) v=$(cat $z/temp 2>/dev/null); [ "$v" -gt "$m" ] 2>/dev/null && [ "$v" -lt 150000 ] && m=$v;; esac; done; echo $m; }
log() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $P/status.log; }
cool() {  # 시작 온도 +1°C 안으로 (최대 600 s)
  s=0; while [ $(cpu_t) -gt $((T0 + 1000)) ] && [ $s -lt 600 ]; do sleep 5; s=$((s + 5)); done
  log "cool waited=${s}s cpu=$(cpu_t) T0=$T0"
}
# 사전 냉각: 화면 끄고 CPU 온도가 1분에 0.5°C 넘게 떨어지지 않을 때까지 (최대 5분) → 그 온도가 T0
pre() { a=$(cpu_t); s=0; while [ $s -lt 300 ]; do sleep 60; s=$((s + 60)); b=$(cpu_t); [ $((a - b)) -lt 500 ] && break; a=$b; done; log "precool ${s}s cpu=$(cpu_t)"; }
input keyevent 223; sleep 20; pre
T0=$(cpu_t); log "start T0=$T0 level=$(dumpsys battery | grep -m1 level)"
EOS
  for r in $(seq $((${R0:-0} + 1)) $((${R0:-0} + ROUNDS))); do
    for c in "${CFGS[@]}"; do
      IFS='|' read -r name bin args <<<"$c"
      L="${EXP}_r${r}_${name}"
      echo "cool"
      echo "./burstpipe_cli --idle $SECONDS_IDLE --label ${L}_idle --power-json \$P/${L}_idle.json >> \$P/cli.log 2>&1"
      echo "log run $L; ./$bin --in $BURST --out \$D/out_power.ppm $args --seconds $SECONDS_WORK --label $L --power-json \$P/$L.json >> \$P/cli.log 2>&1; log done $L rc=\$?"
    done
  done
  echo 'log "end level=$(dumpsys battery | grep -m1 level)"; echo done > $P/DONE'
} > "$R"
$ADB push "$R" $D/power/run_$RUN.sh >/dev/null; rm -f "$R"

# 샘플링 시작 → 러너
# MIUI: 앱 기본값이 백그라운드 제한(RUN_ANY_IN_BACKGROUND ignore) → "startForeground() not allowed due to bg restriction"
#       으로 서비스가 포그라운드가 못 되고 곧 동결돼 샘플이 끊긴다. 절전 예외 + appops 허용.
$ADB shell "dumpsys deviceidle whitelist +dev.burstpipe >/dev/null; cmd appops set dev.burstpipe RUN_ANY_IN_BACKGROUND allow; cmd appops set dev.burstpipe RUN_IN_BACKGROUND allow"
if [ "$SAMPLER" = shell ]; then
  $ADB shell "CLASSPATH=\$(pm path dev.burstpipe | cut -d: -f2) nohup app_process /system/bin dev.burstpipe.ShellSampler $D/power/$RUN.csv 50 > $D/power/$RUN.sampler.log 2>&1 &"
else
  $ADB shell "am start-foreground-service -n dev.burstpipe/.PowerService --es name $RUN --ei period_ms 50 >/dev/null"
fi
sleep 2
$ADB shell "nohup sh $D/power/run_$RUN.sh > $D/power/run_$RUN.log 2>&1 &"
echo "[$DEV $EXP] run=$RUN rounds=$ROUNDS cfgs=${#CFGS[@]} started"
while ! $ADB shell "test -f $D/power/$RUN/DONE" 2>/dev/null; do
  sleep ${POLL:-60}
  $ADB shell "tail -1 $D/power/$RUN/status.log" 2>/dev/null | tr -d '\r' || echo "(adb 연결 끊김? 재시도)"
done
if [ "$SAMPLER" = shell ]; then CSV=$D/power/$RUN.csv; $ADB shell "touch $CSV.stop"; else CSV=/sdcard/Android/data/dev.burstpipe/files/power/$RUN.csv; $ADB shell "am start-foreground-service -n dev.burstpipe/.PowerService --es cmd stop >/dev/null"; fi
for i in $(seq 1 20); do $ADB shell "test -f $CSV.done" && break; sleep 1; done

mkdir -p "$RAW"
$ADB pull $D/power/$RUN/. "$RAW/" >/dev/null
$ADB pull $CSV "$RAW/samples.csv" >/dev/null
python tools/power_analyze.py analyze --csv "$RAW/samples.csv" --records "$RAW" --calib "$OUT/calibration.json" --out "$OUT/$RUN.json"
