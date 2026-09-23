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

## 4. 기기 [C55] — 비어 있음 (hardware_test.md)

| 단계 | 설정 | gray_pyramid | align | merge | finish | mask_refine | disc_blur | composite | total |
|---|---|---|---|---|---|---|---|---|---|
| L0 | 1스레드 scalar | | | | | | | | |
| L1 | 4스레드 big(4-7) 고정 | | | | | | | | |
| L1' | 4스레드 little(0-3) 고정 | | | | | | | | |
| L1'' | 8스레드 미고정 | | | | | | | | |
| L3 | NEON (arm64 빌드 기본) | | | | | | | | |

세그: XNNPACK vs GPU — `seg_infer_parallel`, init, `seg_wait`, 그때의 merge ms
셔터→JPEG (앱, 연속 5회):
Perfetto 코어 점유 스크린샷 / simpleperf 상위 10:

## 5. 서술 ("X→Y ms, 원인 Z") — 기기 수치로 채울 것
1. (PC-emu 예비) merge 타일 선택을 "정렬 타일 1개의 err" → "footprint 후보 최대 4개 중 최소 SAD"로 바꾸자 조명 경계의 밝기 번짐(최대 +450 DN)이 사라지고 삼각대 RMSE 4.58 → 1.65 DN (8장 평균 이론치).
2. (AVD 예비) HAL noise_profile a=1.0을 그대로 쓰면 움직임 거부가 꺼짐(mean_weight 0.999) → 타당성 검사 후 추정으로 대체해 0.815.
3.
4.
5.

## 6. 하지 않은 것과 이유
- fp16 merge: raw 10bit × 가중합 8 = 8184까지 누적, fp16 가수 11bit → 4096 이상에서 정수 해상도 4 → ±2 LSB 양자화가 SNR 측정을 갉아먹음. fp16은 보케 블러에만.
- 서브픽셀 정렬, 주파수 영역 Wiener 합성: v2 (위 1장 "서브픽셀 잔차"가 정량 근거).
- Camera2 NDK 포팅: Kotlin 캡처 + direct ByteBuffer JNI로 충분 (복사 1회).
- Vulkan 보케, QNN HTP, ADPF/서멀: 설계문서 부록 A (4주차 선택).
