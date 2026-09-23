# 실기기(Galaxy C55) 테스트 체크리스트

에뮬레이션으로 검증 가능한 것은 모두 끝났다 (`docs/measurements.md` 3장). 아래는 **실기기가 있어야만** 의미 있는 항목이다.
순서대로 하면 된다. 각 항목의 결과를 `docs/measurements.md` 4·5장에 채운다.

준비 (Windows Git Bash 기준, WSL도 동일)
```bash
export NDK=$LOCALAPPDATA/Android/Sdk/ndk/27.0.12077973
export ADB=$LOCALAPPDATA/Android/Sdk/platform-tools/adb
$ADB devices          # 에뮬레이터를 끄거나 ANDROID_SERIAL=<기기> 지정
```

## 1. 카메라 능력 확인 (개발문서 Task 1 Step 1) — 5분
```bash
$ADB shell dumpsys media.camera > docs/camera_dump.txt
grep -E "HARDWARE_LEVEL|supportedHardwareLevel|availableCapabilities|colorFilterArrangement|whiteLevel|noiseProfile" -A2 docs/camera_dump.txt
```
- [ ] 후면 메인에 `RAW` 능력 — 없으면 설계문서 2장 YUV 폴백 (현재 코드 범위 밖)
- [ ] `MANUAL_SENSOR` 유무 — 있으면 앱이 자동으로 수동 노출(EV −1.5), 없으면 AE_LOCK 경로 (에뮬레이터에서 검증됨)
- [ ] RAW 크기 4080×3060 근처인지, 50MP 풀이면 2×2 비닝 필요 (현재 코드는 비닝 없음 → 메모리 8×100MB 주의)
- [ ] noise_profile 값이 `noise_profile_plausible()` 범위(a≤0.05, b≤0.01) 안인지

## 2. 앱 설치 + 실제 버스트 4세트 덤프 (Task 1 Step 5)
```bash
cd android && ./gradlew assembleDebug && cd ..
$ADB install -r -g android/app/build/outputs/apk/debug/app-debug.apk
```
정적(삼각대)·움직임(손 흔들기)·저조도·인물 각각 앱에서 **DUMP** 버튼 → 회수:
```bash
$ADB pull /sdcard/Android/data/dev.burstpipe/files/ bursts/device/
python3 tools/view_raw.py bursts/device/burst_<ts> 0 check.png    # 색이 초록/보라면 cfa 확인
```
- [ ] 프레임 드롭 0 (logcat `burst done frames=8 failed=0 results=8`)
- [ ] `meta.txt`의 wb_gains/ccm이 1/단위행렬이 아닌 실제 값 (에뮬레이터 HAL은 단위값이었다)

## 3. PC 품질 수치 (Task 8) — 실제 덤프로 교체
```bash
wsl -d Ubuntu -- bash scripts/build_pc.sh
build-pc/burstpipe_cli --in bursts/device/<정적> --out out/s8.ppm --dump-merged out/s8.raw16
build-pc/burstpipe_cli --in bursts/device/<정적> --out out/s1.ppm --frames 1 --dump-merged out/s1.raw16
python3 tools/snr.py out/s1.raw16 out/s8.raw16 <W> <H> <x> <y> 200 200    # 평탄 영역(벽/종이)은 view_raw PNG에서
```
- [ ] SNR 이득 dB (에뮬: 9.6), 움직임 세트 `--dump-weights`로 고스트 확인

## 4. 기기 CPU 측정 L0/L1 (Task 9) — 앱 없이 cli
```bash
bash scripts/build_arm64.sh
$ADB shell "cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq"   # 4~7이 A710인지 확인
S=bursts/device/<정적>
bash scripts/bench_device.sh $S --threads 4 --cpus 0,1,2,3 --repeat 3  ; cp build-arm64/t.json out/t_L1little.json
bash scripts/bench_device.sh $S --threads 8 --repeat 3                 ; cp build-arm64/t.json out/t_L1free.json
```
L0는 NEON을 끈 빌드로 따로 잰다 (arm64 기본 빌드는 NEON = L3):
```bash
NO_NEON=1 bash scripts/build_arm64.sh
BUILD=build-arm64-scalar bash scripts/bench_device.sh $S --threads 1 --repeat 3 ; cp build-arm64-scalar/t.json out/t_L0.json
BUILD=build-arm64-scalar bash scripts/bench_device.sh $S --threads 4 --cpus 4,5,6,7 --repeat 3 ; cp build-arm64-scalar/t.json out/t_L1.json
bash scripts/bench_device.sh $S --threads 4 --cpus 4,5,6,7 --repeat 3 ; cp build-arm64/t.json out/t_L3.json   # NEON
python3 tools/plot_timings.py L0=out/t_L0.json L1=out/t_L1.json little=out/t_L1little.json free=out/t_L1free.json L3=out/t_L3.json
```
- [ ] `test_neon_eq`를 실기기에서: `$ADB push build-arm64/test_neon_eq /data/local/tmp/bp/ && $ADB shell /data/local/tmp/bp/test_neon_eq`

## 5. 프로파일링
```bash
bash scripts/trace_device.sh $S --threads 4 --cpus 4,5,6,7 --repeat 10   # → ui.perfetto.dev
bash scripts/perf_device.sh  $S --threads 4 --cpus 4,5,6,7                # simpleperf 핫스팟 + cache-miss
```
- [ ] 워커가 cpu4~7에만 있는가, merge 구간 코어 4개 100%인가, 클럭 유지/서멀 저하 시작점

## 6. 세그멘테이션 LiteRT (Task 11)
```bash
bash scripts/fetch_litert.sh        # third_party/litert + models/selfie_segmenter.tflite (다운로드 포함)
bash scripts/build_arm64.sh         # BP_HAVE_LITERT 켜짐 → seg_litert.cc 컴파일 (에뮬에서는 컴파일 검증 못 함)
$ADB push models/selfie_segmenter.tflite /data/local/tmp/bp/
P=bursts/device/<인물>
bash scripts/bench_device.sh $P --threads 4 --cpus 4,5,6,7 --repeat 3 --seg-model selfie_segmenter.tflite --delegate cpu
bash scripts/bench_device.sh $P --threads 4 --cpus 4,5,6,7 --repeat 3 --seg-model selfie_segmenter.tflite --delegate gpu
```
- [ ] `seg_infer_parallel` CPU vs GPU, init ms, `seg_wait`≈0, CPU 델리게이트일 때 merge가 느려지면 `--seg-cpu 0`
- [ ] GPU가 adb shell에서 실패(/dev/kgsl-3d0)하면 앱에서 측정 (앱은 assets의 모델 + GPU 델리게이트로 init)
- [ ] 가이디드 필터 전/후 마스크: `--dump-alpha` + 실제 모델 mask.bin으로 경계 비교
- [ ] (선택) QNN HTP: 반나절 상한

## 7. 앱 셔터→JPEG (Task 12)
- [ ] SHOOT 5회 연속, 화면/ logcat `RESULT shutter→jpeg ...ms` 기록 (2회차부터 안정적인지)
- [ ] `$ADB shell dumpsys meminfo dev.burstpipe | grep -E "Native Heap|TOTAL"` — 400~500MB 예상

## 에뮬레이션으로 이미 끝난 것 (다시 할 필요 없음)
- 파이프라인 정확성(단위 7개 + 수용 테스트), NEON==scalar(arm64 번역 실행), NDK 빌드, adb 측정 스크립트 흐름,
  Camera2 RAW 버스트·타임스탬프 매칭·덤프 형식·JNI 무복사 전달·결과 JPEG, 세그 스레드 병렬 합류.
