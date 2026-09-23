#!/usr/bin/env bash
# 에뮬 버스트로 설계문서 Task 8의 "첫 결과"를 재현: 단일/합성/보케 이미지, SNR, 마스크 IoU → out/
set -e
cd "$(dirname "$0")/.."
C=build-pc/burstpipe_cli
T=${THREADS:-8}
mkdir -p out
S=bursts/emu_static; P=bursts/emu_portrait
echo "== static: 8장 합성 vs 1장"
$C --in $S --out out/static_merged.ppm --dump-merged out/static_merged.raw16 --threads $T --json out/t_static.json
$C --in $S --out out/static_single.ppm --frames 1 --dump-merged out/static_single.raw16 --threads $T > /dev/null
python3 tools/snr.py out/static_single.raw16 out/static_merged.raw16 --burst $S 8
echo "== tripod (흔들림 0): SNR 이론치 확인"
$C --in bursts/emu_tripod --out out/tripod_merged.ppm --dump-merged out/tripod_merged.raw16 --threads $T > /dev/null
$C --in bursts/emu_tripod --out out/tripod_single.ppm --frames 1 --dump-merged out/tripod_single.raw16 --threads $T > /dev/null
python3 tools/snr.py out/tripod_single.raw16 out/tripod_merged.raw16 --burst bursts/emu_tripod 8
echo "== motion: 고스트 가중치 맵"
$C --in bursts/emu_motion --out out/motion_merged.ppm --dump-weights out/motion_weights.pgm --threads $T | head -1
echo "== lowlight"
EV=$(awk '/^ev_gain/{print $2}' bursts/emu_lowlight/truth.txt)
$C --in bursts/emu_lowlight --out out/lowlight_merged.ppm --ev $EV --threads $T | head -1
$C --in bursts/emu_lowlight --out out/lowlight_single.ppm --ev $EV --frames 1 --threads $T > /dev/null
echo "== portrait: 에뮬 세그 스레드 → 보케"
$C --in $P --out out/portrait_merged.ppm --threads $T > /dev/null
$C --in $P --out out/portrait_single.ppm --frames 1 --threads $T > /dev/null
$C --in $P --out out/portrait_bokeh.ppm --seg-model $P/mask_emu.bin --delegate emu --dump-alpha out/portrait_alpha.pgm --threads $T --json out/t_portrait.json
python3 tools/mask_iou.py out/portrait_alpha.pgm $P/alpha_gt_q.pgm $P/mask_emu.bin
python3 tools/ppm2png.py out/*.ppm out/*.pgm > /dev/null
python3 tools/compare3.py out/compare_portrait.png out/portrait_single.ppm out/portrait_merged.ppm out/portrait_bokeh.ppm
python3 tools/compare3.py out/compare_static_crop.png out/static_single.ppm out/static_merged.ppm --crop 2200,1800,700,500
python3 tools/compare3.py out/compare_lowlight_crop.png out/lowlight_single.ppm out/lowlight_merged.ppm --crop 1500,1700,900,600
