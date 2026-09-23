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
- 움직임(#4): 참조 선택이 가장 선명한 프레임(2)을 골라 손이 단일 frame 0보다 선명. 손 궤적은 가중치 0(검정) → 다중 손 고스트 없음. 거부 영역은 단일 프레임 노이즈(색 노이즈)가 남음 — HDR+ 공간 합성의 한계, 주파수 합성(v2)이 다룬다. `docs/img/c55_motion.jpg`
- 색: ForwardMatrix CCM으로 흰 벽·피부가 자연스러움 (결과 CCM 단위행렬 그대로면 채도 저하). `docs/img/c55_portrait_highlight.jpg`
- RAW가 매우 어둡다 (중앙값 블랙+15 DN): EV −1.5 + 실내. 저신호 구간은 읽기 노이즈 지배.

### 아직 안 한 것
- LiteRT 세그 CPU vs GPU + 실제 인물 보케 (`scripts/fetch_litert.sh` 다운로드 필요)

## 5. 서술 ("X→Y ms, 원인 Z") — 기기 수치로 채울 것
1. (PC-emu 예비) merge 타일 선택을 "정렬 타일 1개의 err" → "footprint 후보 최대 4개 중 최소 SAD"로 바꾸자 조명 경계의 밝기 번짐(최대 +450 DN)이 사라지고 삼각대 RMSE 4.58 → 1.65 DN (8장 평균 이론치).
2. (AVD 예비) HAL noise_profile a=1.0을 그대로 쓰면 움직임 거부가 꺼짐(mean_weight 0.999) → 타당성 검사 후 추정으로 대체해 0.815.
3. (C55) merge 901 → 280 ms (scalar 1→4 big, 3.2배) → 165 ms (NEON, 1.7배). L0 대비 5.5배.
4. (C55) 4스레드 little 고정은 big 대비 5.3배 느림(2360 vs 442 ms). 8스레드 미고정(373 ms)이 4 big 고정(442 ms)보다 빠름 — A510이 타일을 "물고 늘어진다"는 설계문서 가설과 반대. 동적 작업 큐(원자 카운터)라 느린 코어가 적게 가져가기 때문. NEON에서는 차이 8%(271 vs 295).
5. (C55) 합성 후보 재평가(PC-emu 서술 1의 품질 수정)가 `tile_sad_neon` 31%의 상당 부분 — 품질과 속도의 교환. 다음 최적화 대상.

## 6. 하지 않은 것과 이유
- fp16 merge: raw 10bit × 가중합 8 = 8184까지 누적, fp16 가수 11bit → 4096 이상에서 정수 해상도 4 → ±2 LSB 양자화가 SNR 측정을 갉아먹음. fp16은 보케 블러에만.
- 서브픽셀 정렬, 주파수 영역 Wiener 합성: v2 (위 1장 "서브픽셀 잔차"가 정량 근거).
- Camera2 NDK 포팅: Kotlin 캡처 + direct ByteBuffer JNI로 충분 (복사 1회).
- Vulkan 보케, QNN HTP, ADPF/서멀: 설계문서 부록 A (4주차 선택).
