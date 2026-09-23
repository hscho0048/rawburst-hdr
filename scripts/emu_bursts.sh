#!/usr/bin/env bash
# 에뮬레이터로 설계문서의 4세트(정적/움직임/저조도/인물)를 bursts/ 에 만든다. 실제 덤프가 생기면 이 단계만 교체.
#   bash scripts/emu_bursts.sh [full|half|small]
set -e
cd "$(dirname "$0")/.."
SIZE=${1:-full}
E=build-pc/burstpipe_emu
mkdir -p bursts
$E --out bursts/emu_static   --scene static   --size $SIZE --seed 1 --clean
$E --out bursts/emu_motion   --scene motion   --size $SIZE --seed 2
$E --out bursts/emu_lowlight --scene lowlight --size $SIZE --seed 3
$E --out bursts/emu_portrait --scene portrait --size $SIZE --seed 4
$E --out bursts/emu_tripod   --scene static   --size $SIZE --seed 5 --shake 0 --clean
