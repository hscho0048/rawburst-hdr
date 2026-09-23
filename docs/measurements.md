# 측정 기록

> **출처 표기 규칙**: 이 문서의 모든 숫자는 어디서 쟀는지 표시한다.
> - **[PC-emu]** x64 PC(WSL2, i7-1360P 16스레드) + 센서 에뮬레이터 버스트. 알고리즘 품질·구조 검증용, 기기 성능 아님.
> - **[AVD]** Android 에뮬레이터(API 36 x86_64, arm64는 번역 실행). 흐름 검증용, **시간 수치는 의미 없음**.
> - **[C55]** Galaxy C55 실측 — 아직 없음. `docs/hardware_test.md` 순서대로 채운다.

입력: `scripts/emu_bursts.sh full` → 4080×3060 RAW16(10bit, 블랙 64, RGGB) × 8장. Galaxy C55 후면 50MP Quad-Bayer의 2×2 비닝 RAW를 가정한 에뮬레이터 세트.
노이즈 모델: Camera2 `SENSOR_NOISE_PROFILE` 형식 σ² = a·x + b (base ISO 100에서 a=1.5e-4 ≈ full well 6600e⁻, ISO 비례).

## 1. 품질 [PC-emu]

| 항목 | 값 | 조건 / 도구 |
|---|---|---|
| SNR 이득 (정적, 손떨림 σ=2.5px) | **9.60 dB** | 회색 카드 평탄 영역, 4개 CFA 위상 평균. `tools/snr.py --burst bursts/emu_static` |
| SNR 이득 (삼각대) | 9.59 dB | 이론 √8 = 9.03 dB. 이론을 약간 넘는 이유: 평탄 타일의 모션 벡터가 노이즈로 정해져 겹침 타일끼리 서로 다른 픽셀을 섞음 = 약한 공간 평활 |
| 합성 RMSE vs 노이즈 없는 정답 (삼각대, 1024×768) | 단일 4.55 → 합성 **1.65 DN** | 8장 평균 이론치(×0.354)와 일치. `test_pipeline` |
| 움직임: 가중치 (손이 지나간 영역 / 나머지) | **0.60 / 0.92** | 손 영역 합성 RMSE 6.19 → 4.81 (단일보다 작음 = 고스트 없음). `docs/img/motion_weights.png` |
| mean_weight | 정적 0.968 / 움직임 0.948 / 저조도 0.986 / 인물 0.976 | cli 출력 |
| 마스크 IoU vs GT (인물, 1/4 해상도) | 업샘플 0.986 / 가이디드 0.983 | `tools/mask_iou.py`. 아래 "해석" 참고 |
| 세그 대기 (`seg_wait`) | **0.0 ms** (에뮬 지연 40 ms 넣어도 0.0) | 세그 스레드가 정렬·합성 뒤에 완전히 숨음 |

해석
- **서브픽셀 잔차**: 손떨림이 있으면 강한 엣지(조명, 시멘스 스타)에서 합성 RMSE가 단일보다 커진다. v1 정렬은 gray 정수(= Bayer 2px) 단위라 ±1px 잔차가 남아 엣지가 살짝 블러된다. 설계문서 5.2의 "서브픽셀은 v2" 결정이 만든 비용이고, 에뮬레이터 정답(`clean_NN.raw16`)으로 정량화할 수 있다.
- **마스크 IoU**: 에뮬 마스크는 "머리카락만 빠진 정답 실루엣을 블러한 것"이라 이미 매우 정확하다. 가이디드 필터가 IoU를 올리지 못하는 것은 가이드(휘도)에서 피부와 배경 밝기가 비슷한 구간 때문. 실제 모델 마스크(256px, 경계 수 px 오차)에서 전/후를 다시 재야 한다 → hardware_test.md 6번.

## 2. 단계별 ms [PC-emu] — 스레드 스윕 (`scripts/bench_pc.sh`, `--repeat 3` 마지막 회차)

| 설정 | gray_pyramid | select_ref | align | merge | finish | seg_wait | seg_infer_parallel | mask_refine | disc_blur | composite | total |
|---|---|---|---|---|---|---|---|---|---|---|---|
| L0 1스레드 | 71.5 | 2.1 | 1079.8 | 1866.9 | 268.7 |  |  |  |  |  | 3288.9 |
| 2스레드 | 35.8 | 3.6 | 584.7 | 975.0 | 129.9 |  |  |  |  |  | 1729.0 |
| L1 4스레드 | 47.7 | 1.9 | 340.9 | 654.7 | 86.1 |  |  |  |  |  | 1131.4 |
| 8스레드 | 23.2 | 1.9 | 208.0 | 376.2 | 43.9 |  |  |  |  |  | 653.2 |
| 인물 4스레드 | 33.2 | 5.3 | 324.9 | 658.6 | 72.0 | 0.0 | 3.5 | 57.8 | 186.2 | 183.1 | 1521.2 |
| 인물 4스레드 + 세그 지연 40ms | 27.4 | 4.9 | 352.7 | 675.4 | 92.0 | 0.0 | 44.6 | 59.7 | 196.5 | 208.0 | 1616.6 |

- total = 임계 경로 합 (`*_parallel`은 세그 스레드라 제외).
- 노트북(전원 상태·터보)에 따라 ±40% 흔들린다. 같은 세션 안의 **상대비**만 본다: 1→4스레드 2.9배, 1→8스레드 5.0배.
- merge가 가장 크다 → 기기에서 L2(패스 융합은 이미 적용) → L3 NEON 순서의 근거. 기기에서 simpleperf로 확인할 것.

## 3. 기기 경로 [AVD] — 흐름만 검증

| 항목 | 결과 |
|---|---|
| NDK r27 arm64-v8a / x86_64 빌드 | 경고 없이 성공 (`scripts/build_arm64.sh`, `ABI=x86_64`) |
| 단위 테스트 7개 on AVD | x86_64 전부 통과, **arm64(번역 실행, NEON 경로 ON) 전부 통과** |
| `test_neon_eq` (arm64) | tile_sad bit-exact, merge_row 상대오차 0 |
| 전체 파이프라인 NEON vs `NO_NEON=1` scalar (arm64, 2040×1528 인물 8장) | merged.raw16·결과 PPM **md5 동일 (bit-exact)** |
| `bench_device.sh` / `--delegate emu` | adb push → 실행 → t.json 회수 동작 |
| `dumpsys media.camera` (AVD) | 후면(ID 10) FULL, RAW, RGGB, white 1023, **MANUAL_SENSOR 없음** → 앱은 AE_LOCK+EV 보정 경로 |
| 앱: EMU 버스트 → JNI → 에뮬 세그 → 보케 → JPEG | 동작 (2040×1528) |
| 앱: Camera2 RAW 버스트 8장 → 처리 → JPEG | 동작. 1280×960, 드롭 0, 타임스탬프 매칭 8/8 |
| 앱: DUMP → `adb pull` → PC cli | 동작 (`out/device_dump`). `docs/img/android_emulator_raw_burst.jpg` |
| AVD HAL noise_profile | **a=1.0 (엉터리)** → 그대로 쓰면 고스트 거부가 꺼짐(mean_weight 0.999). `noise_profile_plausible()`로 무시하고 정렬 오차 추정으로 대체 → 0.815 |

## 4. 기기 [C55] — Galaxy C55 SM-C5560, Android 16, SM7450(taro)

카메라 (`docs/camera_dump.txt`, 후면 메인 ID 0): FULL, RAW + MANUAL_SENSOR + BURST_CAPTURE, RAW_SENSOR **4080×3072**(2×2 비닝),
CFA **GBRG**(cfa=2), white 1023, black 64, ISO 100–6400. 결과 `noise_profile` a=4.4e-4 b=0 @ISO106 (타당 범위).
**결과 CCM이 단위행렬** → 앱이 ForwardMatrix1(D65) × XYZ(D50)→sRGB로 CCM을 계산해 `meta.txt`에 기록 (`ccm_source forward_matrix`).
CPU: 0–3 A510 1.80 GHz, 4–6 A710 2.36 GHz, 7 A710 2.40 GHz.

### 단계별 ms (cli, `bench_device.sh`, 입력 `bursts/device/test0` 4080×3072×8, `--repeat 3` 마지막)

| 단계 | 설정 | gray_pyramid | align | merge | finish | total |
|---|---|---|---|---|---|---|
| L0 | scalar 1스레드 (cpu7) | 25.9 | 292.0 | 901.4 | 203.6 | **1424.8** |
| L1 | scalar 4스레드 big(4-7) | 24.7 | 82.5 | 279.7 | 52.4 | 441.5 |
| L1' | scalar 4스레드 little(0-3) | 39.6 | 436.6 | 1461.5 | 414.9 | 2359.6 |
| L1'' | scalar 8스레드 미고정 | 25.0 | 72.2 | 225.1 | 48.6 | 372.9 |
| L3 | NEON 1스레드 (cpu7) | 26.8 | 153.4 | 535.3 | 225.9 | 943.6 |
| L3 | **NEON 4스레드 big** | 24.8 | 45.4 | 164.5 | 58.2 | **295.1** |
| L3 | NEON 8스레드 미고정 | 25.1 | 41.6 | 148.5 | 53.5 | 270.8 |

(원본 JSON: `out/c55/t_*.json`. 설계문서 7.1 추정 L0 ~3.3s(보케 제외) → 실측 1.42s, L4 목표 ~0.37s → NEON 4big 0.30s)

### simpleperf (NEON 4big, `cpu-cycles:u` — 삼성 user 빌드는 커널 샘플·소프트웨어 이벤트 금지)

| 심볼 | 비중 |
|---|---|
| `tile_sad_neon` (정렬 + 합성 후보 재평가) | 31.2% |
| `merge_row_neon` | 20.5% |
| `finish` 디모자이크 람다 | 17.3% |
| `merge_burst` 타일 루프 (타일 평균·후보·행 포인터) | 11.7% |
| `bayer_to_gray` | 4.6% |
| `align_frame` 루프 | 3.2% |

IPC 2.66 (12.95G inst / 4.87G cycles), L1D miss 1.8억, LLC miss 4200만, backend stall 25%.

### Perfetto (`trace_device.sh`, `out/c55/trace.txt`)
align/merge/finish 구간 워커는 cpu4–7에만, 점유 85–100%, 클럭 2361/2400 MHz 유지. little 0%. ATrace 슬라이스 정상 표시.

### 앱 셔터→JPEG (SHOOT 연속 5회, 4080×3072 × 8, ISO ~330 실내)
| 회차 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| 셔터→JPEG ms | 1086 | 1108 | 1135 | 1043 | 1129 |
| 캡처 ms | 470 | 501 | 485 | 488 | 498 |
| 처리 ms (JNI) | 394 | 403 | 389 | 341 | 364 |

나머지 ~200 ms = Bitmap + JPEG(95) 인코딩. 1회차부터 안정. 메모리 PSS 609 MB (Native Heap 327 MB, direct ByteBuffer 슬롯 196 MB 별도).

### 실제 장면 품질 (앱 DUMP 6세트 → PC cli, 4080×3072 × 8, 핸드헬드)

| # | 장면 | 노출 | mean_weight | SNR 이득 (`tools/snr_auto.py`) |
|---|---|---|---|---|
| 1 | 책상 정적 | 11.8 ms ISO 569 | 0.945 | **8.13 dB** (평탄 블록 IQR 7.96–8.29) |
| 2 | 책상 + 손(가장자리) | 14.7 ms ISO 463 | 0.938 | 7.71 dB |
| 3 | 밝은 패널, 어두운 방 | 2.9 ms ISO 150 | 0.996 | 6.16 dB (어두운 영역, 신호 4–15 DN) |
| 4 | 손 흔들기 | 14.7 ms ISO 266 | **0.648** | — (고스트 확인용) |
| 5 | 인물(거울) | 14.1 ms ISO 250 | 0.993 | 7.86 dB |
| 6 | 저조도 | 33.3 ms **ISO 6400** | 0.997 | **9.86 dB** |

- 8장 이론 9.03 dB. 정적 핸드헬드 8.1 dB = 이론의 90%. 저조도가 이론을 넘는 것은 PC-emu와 같은 원인(평탄부 모션 노이즈에 의한 약한 공간 평활).
- `snr_auto.py` 검증: 에뮬 정적 세트에서 9.65 dB vs 정답 영역 기반 `snr.py` 9.60 dB.
- 움직임(#4): 참조 선택이 가장 선명한 프레임(2)을 골라 손이 단일 frame 0보다 선명. 손 궤적은 가중치 0(검정) → 다중 손 고스트 없음. `docs/img/c55_motion_weight.jpg` 거부 영역은 단일 프레임 노이즈(색 노이즈)가 남음 — HDR+ 공간 합성의 한계, 주파수 합성(v2)이 다룬다.
- 색: ForwardMatrix CCM으로 흰 벽·피부가 자연스러움 (결과 CCM 단위행렬 그대로면 채도 저하).
- RAW가 매우 어둡다 (중앙값 블랙+15 DN): EV −1.5 + 실내. 저신호 구간은 읽기 노이즈 지배.

### 세그멘테이션 LiteRT 2.16.1 (MediaPipe selfie_segmenter float16, 인물 덤프 #5)

| 경로 | init | seg_infer_parallel | seg_wait | 부작용 |
|---|---|---|---|---|
| cli CPU (XNNPACK 4스레드 + 자체 커스텀 op) | 308 ms | 370–380 ms | 0.0 | big 코어 경쟁: align 47 → 257 ms, total 502 → 837 ms |
| cli CPU, 세그 스레드 little(`--seg-cpu 0`) | — | 256 ms | 0.0 | align 188 ms, total 688 ms (XNNPACK 워커는 고정 안 됨) |
| cli GPU (OpenCL, 246/246 노드 위임) | 2.2–2.4 s | **19–31 ms** | 0.0–0.1 | 없음. total 502 ms (보케 포함) |
| 앱 GPU, OpenGL 폴백 | 0.23 s | 32–50 ms | 0.04 | — |
| 앱 GPU, OpenCL (`uses-native-library libOpenCL.so`) | 2.7 s (앱 시작 시 백그라운드) | **19–35 ms** | 0.04 | — |

- 앱 셔터→JPEG (인물모드, 보케 포함, 연속 5회): **1238–1316 ms** (캡처 ~490 + 처리 ~610 + JPEG ~150)
- 커스텀 op 검증: 자체 CPU 구현 마스크 vs GPU 델리게이트 내장 구현 마스크 **IoU 0.998**, 평균 |Δα| 0.001
- 방향 수정 전/후 (같은 버스트): 전경 비율 2.9% → **15.2%** (전신). `docs/img/c55_seg_mask.jpg`

실기기에서 찾은 통합 버그 3개
1. **커스텀 op**: selfie_segmenter는 `Convolution2DTransposeBias`(MediaPipe 전용)를 써서 표준 TFLite가 로드 실패 → C API `TfLiteInterpreterOptionsAddCustomOp`로 전치 합성곱 + bias 커널 등록.
2. **센서 방향**: RAW는 센서 방향(90°) 그대로라 모델이 옆으로 누운 사람을 받음 → 머리·상체만 검출. `meta.txt`에 `orientation` 추가, 모델 입력을 정립 회전하고 마스크를 되돌림 (`test_seg` 왕복 테스트).
3. **델리게이트 스레드 친화성**: 앱에서 "GpuDelegate must run on the same thread where it was initialized" → 보케가 조용히 꺼짐. 생성·추론·해제를 전용 스레드 1개가 소유 (`ThreadBoundSegmenter`, 설계문서 6장). cli는 우연히 통과했었다.

## 5. 선택 항목 (설계문서 부록 A / L5·L7·NPU·품질 v2) [C55 + PC-emu]

### 5.1 합성 최적화 — 측정으로 기각한 2개
| 시도 | 가설 | C55 NEON 4big merge | 판정 |
|---|---|---|---|
| 기준 (타일 우선, 전역 num/den) | — | 165–193 ms | 유지 |
| 후보 SAD 행 쌍 솎기 (비용 ½) | simpleperf `tile_sad` 31% → 줄이면 빨라진다 | 170 ms (변화 없음) | 기각 |
| 밴드 스트리밍 (전역 num/den 100MB 제거, L2 상주 누적기) | 대역폭 병목 제거 | 205 → 251–265 ms (**느려짐**) | 기각 |

원인 (simpleperf 비교): 새 구조는 명령어 16.5G → 15.3G로 **적은데** 사이클 6.4G → 7.8G, IPC 2.59 → 1.96, LLC miss 5500만 → 8400만.
기존 구조에서는 **타일 전체 SAD가 그 타일 32행 × 8프레임을 캐시로 끌어오는 프리페치 역할**을 해서 바로 다음 누적(`merge_row`)이 캐시에서 읽었다.
솎기·패스 분리가 그 지역성을 깼다 → `merge_row_neon` 비중 18.5% → 34%. 교훈: "핫스팟 함수 비용"과 "그 함수가 만드는 캐시 상태"는 따로 봐야 한다.

### 5.2 서브픽셀 정렬 — 측정으로 기각
- 구현: 최하단 SAD 곡면 등각 직선 맞춤(추정 정확도: 참 0.25/0.5/3.3 px → 0.249/0.500/3.273) + 같은 CFA 색 평면 쌍선형 적용.
- 결과 (`tools/rmse_clean.py`, 에뮬 정답 대비 RMSE DN, 4080×3060×8):

| | 전체 | 엣지(그래디언트 상위 10%) | 평탄 |
|---|---|---|---|
| 손떨림, 정수 정렬 | 3.27 | **7.39** | 2.35 |
| 손떨림, 서브픽셀 | 3.56 | 8.55 | 2.33 |
| 삼각대, 정수 | 1.62 | **1.65** | 1.63 |
| 삼각대, 서브픽셀 | 1.82 | 3.50 | 1.50 |

- 반해상도 색 평면(이웃 간격 raw 4px)에서의 보간은 alt 프레임을 흐리게 만들어, 선명한 ref와 섞일 때 엣지 오차가 오히려 커진다.
  SNR 수치는 11.7 dB로 "좋아 보이지만" 그건 보간에 의한 평활이다. HDR+(2016)가 Bayer 합성을 정수 정렬로 둔 이유.
  제대로 하려면 Wronski 2019(커널 회귀 초해상도). → 코드 되돌림.

### 5.3 보케 — 알고리즘이 GPU보다 먼저
| 단계 | 이전 | 이후 | 방법 |
|---|---|---|---|
| disc_blur (반지름 12, 1020×768) | 96 ms (CPU 직접 합산 ~450탭) | **17 ms** | 행 누적합: 원 = 행별 구간합 → 픽셀당 25행×2 조회. 직접 합산 대비 max 차 1.5e-5 (`test_guided`) |
| composite (4080×3072) | 72–80 ms | **28 ms** | 세로 보간을 행 버퍼로 1번, 열 보간 계수 테이블, `std::lround` 제거 (출력 차 ≤1/255) |
| Vulkan compute disc_blur (직접 합산, 공유메모리 타일) | — | 커널 70 ms + 업로드 2 + 다운로드 9 | Adreno 644, 타임스탬프 쿼리. CPU와 출력 차 ≤1/255 |

- L5 결정: 누적합 CPU 17 ms < GPU 전송만 11 ms + 커널 → **GPU 경로는 기본 꺼짐** (`--gpu-blur`로 측정 가능하게 유지).
  "GPU 이득이 가장 큰 단계"라던 설계 가정은 O(r²) 알고리즘을 전제로 한 것이었다.
- 인물모드 처리(합성+보케, GPU 세그): 445 → 325 ms.

### 5.4 NPU — Qualcomm AI Engine Direct (QNN) HTP v69
- QAIRT 2.45 `libQnnTFLiteDelegate.so` (dlopen) + `libQnnHtpV69Stub/Skel`. HTP fp16, burst 모드.
- **246개 중 245 노드 위임**, 남는 1개 = MediaPipe 커스텀 op(CPU 자체 커널) → 3 파티션.
- cli: init 0.84–0.96 s, 추론 9.5–24 ms (파티션 왕복 편차). GPU(OpenCL): init 2.2–2.4 s, 19–31 ms.
- 마스크: NPU vs GPU IoU 0.996.
- 앱: `uses-native-library libcdsprpc.so` + skel을 nativeLibraryDir에 풀기(legacy packaging) + `ADSP_LIBRARY_PATH`.
  없으면 "libcdsprpc.so not found → Failed to load skel". 앱 init 1.0–1.1 s (GPU OpenCL 2.7–3.3 s).
- **최종 앱 인물모드 (NPU 세그, Malvar, 누적합 보케, 식은 상태 42.6°C에서 5연속)**: 셔터→JPEG **1122–1270 ms**, 처리 444–527 ms, 세그 13–24 ms, disc_blur 23–26 ms, composite 35–36 ms.

### 5.5 ADPF / 발열 (L7)
- `APerformanceHint`, `AThermal_getThermalHeadroom`은 dlsym으로 연결했으나 **이 기기는 둘 다 미지원**:
  `dumpsys performance_hint` → `Hint Session Support: false`, `dumpsys thermalservice` → 헤드룸 임계값 전부 NaN.
- 150연속 촬영 (앱, 보케 없음, 정책 없음): 1–50장 처리 중앙값 412–414 ms → **81장 이후 515–530 ms (+28%)**, 빅코어 최대 클럭 2.40 → 1.77 GHz.
  그동안 **Thermal Status는 계속 0** — 상태 API는 스로틀의 선행 신호가 아니다.
- 그래서 지연 피드백 거버너(`LatencyGovernor`: 처리 시간 EMA > 목표 450 ms×1.1이면 N−1, <×0.8이면 N+1, 변경 시 EMA 비례 보정):

| 150연속 | 81–110장 처리 중앙값 | 111–150장 | 셔터→JPEG (111–150) | 평균 N |
|---|---|---|---|---|
| 정책 없음 | 515 ms | 530 ms | 1328 ms | 8 |
| 지연 거버너 | 459 ms | **458 ms** | **1235 ms** | 8 → 7 → 6 |

  N 8→6의 대가: 이론 SNR −1.25 dB. 주의: 거버너 측정은 이전 스트레스의 클럭 캡(1.77 GHz)이 남은 상태에서 시작 — 조건이 완전히 같지 않다.

### 5.6 Malvar-He-Cutler 디모자이크 (품질 v2)
- 에뮬 정답 RGB(모자이크 전, 같은 CCM·톤) 대비 PSNR: bilinear 27.4 dB → **Malvar 29.3 dB (+1.8)**, RGGB·GBRG 동일 (`test_demosaic`).
- C55 finish 비용: +12–60 ms (측정 편차 큼, 발열). 기본값 Malvar, `--demosaic bilinear`로 전환.

### 5.6b 최종 앱 인물모드 (변환 모델, Malvar, 누적합 보케, 식힌 상태 ≤43°C·2.4 GHz에서 각 5연속)
| 세그 | init | 셔터→JPEG | 처리 | 세그 추론 | disc_blur | composite |
|---|---|---|---|---|---|---|
| GPU (OpenCL) | 2.6 s | 1108–1230 ms | 445–523 ms | 14–24 ms | 24–26 ms | 34–36 ms |
| **NPU (HTP)** | **1.0 s** | 1096–1211 ms | 437–550 ms | **4.6–11 ms** | 23–26 ms | 35–36 ms |

세그는 두 경우 모두 합성 뒤에 숨어(seg_wait≈0) 셔터→JPEG 차이는 작고, NPU의 이점은 init(2.6 → 1.0 s)과 GPU를 비워 두는 것.

### 5.7 세그 모델 변환 — NPU 파티션 3 → 1
- `tools/convert_selfie_model.py`: MediaPipe 커스텀 op `Convolution2DTransposeBias` → 표준 `TRANSPOSE_CONV` v3 (+output_shape 상수, bias 입력, SAME/stride 2). 모델 1개 노드.
- 검증: 변환 모델 CPU 마스크 vs 원본(자체 커스텀 op 커널) **IoU 1.0000, |Δ| 0.0000** — 변환과 자체 커널 둘 다 표준 TRANSPOSE_CONV와 동일.
- NPU: 246/246 노드, **1 파티션** (원본 245/246, 3 파티션). GPU도 246/246.
- 전처리/추론 분리 측정 후: NPU 순수 추론 원본 3.9–10.6 ms vs 변환 4.7–5.1 ms (편차 감소), 전처리(RAW→256² RGB) 4.5–12.5 ms — `std::pow` 감마를 LUT로 바꿈.
- 앱은 `selfie_segmenter_std.tflite`가 있으면 우선 사용.

### 5.8 주파수 영역 Wiener 합성 (HDR+ §5) — 품질 모드 `--merge wiener`
- 32×32 raw 타일 → Bayer 평면 4개 16×16 DFT, `Az = |D|²/(|D|²+c·σf²)`, c=16. 정렬·후보 선택은 공간 합성과 같다.
- 에뮬 정답 대비 RMSE (DN, 4080×3060×8):

| | 전체 | 엣지 | 평탄 | 움직임 영역 |
|---|---|---|---|---|
| 공간 합성 (손떨림) | 3.27 | 7.39 | 2.35 | 3.69 |
| **Wiener c=16 (손떨림)** | **2.20** | **3.19** | **2.07** | **2.91** |
| 공간 합성 (삼각대) | **1.62** | **1.65** | 1.63 | — |
| Wiener c=16 (삼각대) | 1.89 | 1.83 | 1.92 | — |

  주파수별 수축이 서브픽셀 잔차로 어긋난 고주파 성분만 참조 쪽으로 돌리고 저주파는 평균 → 5.2에서 보간으로 못 푼 엣지 블러를 해결. 정렬이 완벽한 삼각대에서는 약간 손해.
- C55 합성 시간: `std::complex` 5.4 s → 분리 배열 2.74 s → 열 방향 벡터화 FFT 1.35 s → 실수 2장 묶음 FFT **0.99 s** (공간 합성 0.19 s의 5배). 품질 모드로 두고 기본은 공간 합성.
  `std::complex` 곱은 `-ffast-math` 없이 `__mulsc3` 호출(NaN/Inf 처리)로 바뀌어 가장 큰 병목이었다.

### 5.9 Mertens 로컬 톤매핑 — `--ltm`
- 1/4 해상도 휘도에서 합성 노출 2장(×1, ×4) → well-exposedness(σ=0.2) 가중 Laplacian 피라미드 융합 → 픽셀당 선형 게인(1–4) → 풀해상도 쌍선형 적용. 보케용 1/4 영상에도 같은 게인.
- 실사(저조도 방·역광 패널·책상): 암부가 올라오고 하이라이트 유지. 패널 주변 약한 헤일로. PC +45–56 ms. 옵트인.

### 5.10 Camera2 NDK 포팅 — `--es capture ndk`
- `android/ndk_camera.cc`: ACameraManager/AImageReader(RAW16, N+2)/수동 노출 버스트/결과 메타(타임스탬프 매칭, WB·CCM·노이즈·동적 블랙, ForwardMatrix CCM·orientation)를 C++에서. 프레임은 `AImage` 평면 → 네이티브 슬롯 1회 복사, Java 경유 없음.
- C55: 8장 드롭 0, 캡처 476–496 ms (Kotlin 경로 ~480 ms와 같음 — 캡처 시간은 복사가 아니라 센서 프레임 주기 8×33 ms가 지배), 셔터→JPEG 1.08–1.18 s.

### 5.11 초해상도형 합성 (Wronski 2019 단순화) — 실험 옵션 `--merge superres`
- 출력 픽셀마다 모든 프레임의 raw 샘플을 소수 이동(등각 맞춤) 위치에 놓고, 구조 텐서 기반 이방성 가우시안 × 타일 강건성으로 누적 → 디모자이크 겸 합성.
- 에뮬 정답 RGB 대비 PSNR (1024×768×8, `test_superres`, 파라미터는 `BP_SR_SWEEP=1` 탐색):

| | 단일+Malvar | 공간+Malvar | Wiener+Malvar | 초해상도 |
|---|---|---|---|---|
| 손떨림 1.5px | 27.73 | 29.16 | 29.13 | **29.74** |
| 삼각대 | 27.79 | **29.02** | 28.96 | 28.01 |

  등방 커널은 28.05 dB(가중 bilinear 디모자이크와 같음) → 이방성 커널이 핵심. 서브픽셀 다양성(손떨림)이 있어야 이득.
- C55 비용 **6.1 s** (픽셀당 8프레임×25샘플), 실사 크롭에서 체감 선명도 차이는 뚜렷하지 않음 → 실험 옵션.

## 6. 서술 ("X→Y ms, 원인 Z")
1. (PC-emu) merge 타일 선택을 "정렬 타일 1개의 err" → "footprint 후보 최대 4개 중 최소 SAD"로 바꾸자 조명 경계의 밝기 번짐(최대 +450 DN)이 사라지고 삼각대 RMSE 4.58 → 1.65 DN (8장 평균 이론치).
2. (AVD) HAL noise_profile a=1.0을 그대로 쓰면 움직임 거부가 꺼짐(mean_weight 0.999) → 타당성 검사 후 추정으로 대체해 0.815.
3. (C55) merge 901 → 280 ms (scalar 1→4 big, 3.2배) → 165 ms (NEON, 1.7배). L0 대비 5.5배.
4. (C55) 4스레드 little 고정은 big 대비 5.3배 느림(2360 vs 442 ms). 8스레드 미고정(373 ms)이 4 big 고정(442 ms)보다 빠름 — 설계문서 가설과 반대. 동적 작업 큐(원자 카운터)라 느린 코어가 적게 가져가기 때문.
5. (C55) 세그 CPU 델리게이트 370 ms가 합성 스레드와 big 코어를 다퉈 align 47 → 257 ms. GPU(OpenCL) 19–31 ms로 옮기자 seg_wait 0.
6. (C55) 앱 GPU 델리게이트가 OpenGL로 폴백(32–50 ms) → `uses-native-library libOpenCL.so` 후 19–35 ms, init 0.23 → 2.7 s (앱 시작 시 웜업).
7. (C55) disc_blur 96 → 17 ms: O(r²) 직접 합산 → 행 누적합 O(r). 그 결과 Vulkan(전송만 11 ms)의 이점이 사라져 GPU 경로를 기본에서 뺐다.
8. (C55) 합성 "대역폭 최적화"(밴드 스트리밍)가 165 → 251 ms로 역효과: 명령어는 줄었지만 IPC 2.59 → 1.96. SAD가 하던 암묵적 프리페치를 깼기 때문 → 되돌림.
9. (C55) 150연속 촬영에서 81장째부터 처리 +28%(빅코어 2.40 → 1.77 GHz), Thermal Status는 0 유지·헤드룸 API 미지원 → 처리 시간 피드백으로 N을 8→6으로 낮춰 458 ms 유지 (SNR −1.25 dB).
10. (C55) 세그 NPU(HTP) 9.5–24 ms, init 0.84 s vs GPU 19–31 ms, init 2.2 s — 245/246 노드 위임, 남은 커스텀 op 1개가 파티션 왕복 편차의 원인.
11. (C55) 그 커스텀 op를 모델 수술로 표준 TRANSPOSE_CONV로 바꾸자 NPU 246/246·1 파티션, 순수 추론 4.7–5.1 ms (마스크 IoU 1.0).
12. (C55) Wiener 합성 5.4 → 0.99 s: `std::complex`의 `__mulsc3` 호출 제거(2×), 열 방향 벡터화 FFT(2×), 실수 2장 묶음 FFT(1.4×). 품질은 손떨림 엣지 RMSE 7.4 → 3.2 DN.

## 7. 하지 않은 것과 이유
- fp16 merge: raw 10bit × 가중합 8 = 8184까지 누적, fp16 가수 11bit → 4096 이상에서 정수 해상도 4 → ±2 LSB 양자화가 SNR 측정을 갉아먹음.
- Bayer 평면 보간 서브픽셀 합성: 5.2 측정으로 기각. 서브픽셀은 Wiener(5.8)와 초해상도(5.11)가 다른 방식으로 활용.
- 초해상도의 실시간화 (GPU 이식 등): 6.1 s → 품질 이득 대비 비용이 커서 실험 옵션에 머무름.
