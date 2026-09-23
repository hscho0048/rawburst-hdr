#!/usr/bin/env bash
# PC(x64, WSL)에서 기기 측정표와 같은 축으로 측정: 스레드 수 스윕 + 보케. 기기 수치가 아니다 (구조·상대비 확인용).
#   bash scripts/bench_pc.sh [bursts/emu_static] [bursts/emu_portrait]
set -e
cd "$(dirname "$0")/.."
S=${1:-bursts/emu_static}; P=${2:-bursts/emu_portrait}
C=build-pc/burstpipe_cli
O=out/bench_pc; mkdir -p $O
for t in 1 2 4 8; do
  $C --in $S --out $O/x.ppm --threads $t --repeat 3 --json $O/t_static_T$t.json > /dev/null
done
$C --in $P --out $O/x.ppm --threads 4 --repeat 3 --seg-model $P/mask_emu.bin --delegate emu --json $O/t_portrait_T4.json > /dev/null
$C --in $P --out $O/x.ppm --threads 4 --repeat 3 --seg-model $P/mask_emu.bin --delegate emu --seg-emu-latency 40 --json $O/t_portrait_T4_seg40.json > /dev/null
python3 tools/plot_timings.py "L0 1스레드=$O/t_static_T1.json" "2스레드=$O/t_static_T2.json" "L1 4스레드=$O/t_static_T4.json" \
  "8스레드=$O/t_static_T8.json" "인물 4스레드=$O/t_portrait_T4.json" "인물 4스레드+세그40ms=$O/t_portrait_T4_seg40.json"
