# 전력·에너지 측정 — Galaxy C55 / Xiaomi Mi Mix 4

> **모든 전력 수치는 기기 전체(배터리) 기준이다.** 삼성·샤오미 양산 기기에는 Pixel의 ODPM 같은 레일별(SoC/DDR/카메라) 전력계가 없어서
> 배터리에서 나가는 전체 전력을 잰다. SoC 단독 전력이 아니며, 화면·모뎀(비행기 모드)·카메라 모듈·DRAM이 모두 들어 있다.
> 그래서 주 지표는 같은 조건 idle을 뺀 **순 에너지(net J)** 와 그것을 반복 횟수로 나눈 **net J/회**다.
> 실측한 것만 적는다. 추정은 "추정", 안 된 실험은 "안 됨 + 이유"로 표시한다.

출처 표기: **[C55]** Galaxy C55 실측, **[Mix4]** Mi Mix 4 실측. 원본 JSON은 `results/power/<기기>/`.

## 1. 기기

| | Galaxy C55 (SM-C5560) | Xiaomi Mi Mix 4 (2106118C) |
|---|---|---|
| SoC | Snapdragon 7 Gen 1 (SM7450, taro) | Snapdragon 888+ (SM8350, lahaina; X1 2.995 GHz) |
| CPU | 0–3 A510 1.80 GHz / 4–6 A710 2.36 GHz / 7 A710 2.40 GHz | 0–3 A55 1.80 GHz / 4–6 A78 2.42 GHz / 7 X1 2.995 GHz |
| NPU (Hexagon HTP) | v69 — fp16 지원 | **v68 (Hexagon 780) — fp16 미지원, 정수만** |
| OS | Android 16 (One UI) | Android 14, HyperOS OS1.0.8.0 (커널 5.4) |
| 배터리 | 1셀 ~4.3 V | **2셀 직렬 ~8.6 V** (120 W 충전 구조) |
| 화면 | 1080×2400 | 1080×2400 120 Hz |
| 메인 카메라 RAW | 4080×3072 (50MP 2×2 비닝), GBRG, ISO 100–6400 | **6016×4512 (108MP 센서 비닝, RAW는 이 크기 하나뿐)**, GRBG, ISO 50–1600 |
| BatteryManager CURRENT_NOW | µA, **방전 음수** | µA(1 mA 단위로 양자화), **방전 양수** |

(Mix 4 카메라 정적 정보: `docs/camera_dump_mix4.txt`, C55: `docs/camera_dump.txt`)

## 2. 측정 방법

### 2.1 샘플러 — 왜 앱에서 재나
- cli(`core/src/power.cc` `BatterySampler`): 스레드 하나가 `/sys/class/power_supply/*/current_now`·`voltage_now`를 50 ms마다 읽어 P=V×I를 사다리꼴 적분.
  **두 기기 모두 shell에서 `Permission denied`** (battery, usb, ac, wireless, otg 전부 — `results/power/*/raw/*/val_*.json`의 `sysfs_power.why`).
  thermal_zone의 `vbat`(전압)은 읽히지만 전류(`*-ibat-lvl*`)는 BCL 임계값용이라 값이 일관되지 않아(Mix 4에서 lvl0 156 / lvl1 468) 쓰지 않았다.
  `dumpsys battery`의 `current now`(삼성만 있음)는 5초 간격으로 읽어도 같은 값(캐시)이라 샘플링용이 아니다.
- 그래서 **앱 `PowerService`(foreground service)** 가 `BatteryManager.BATTERY_PROPERTY_CURRENT_NOW`를 50 ms마다 읽고, 전압은 `ACTION_BATTERY_CHANGED`의 `EXTRA_VOLTAGE`를 쓴다.
  CSV: `t_ns(CLOCK_BOOTTIME), current_raw, charge_uah, voltage_mv, temp_dc, plugged, status`. 5초마다 파일에 붙인다.
- cli 실험은 cli가 **구간 경계만** 기록한다 (`--power-json`: `CLOCK_BOOTTIME` ns = 앱의 `elapsedRealtimeNanos`와 같은 시간축 + 반복 수·ms·온도·클럭).
  PC에서 `tools/power_analyze.py`가 샘플 × 구간을 적분한다. 앱 실험(E3·E4)은 앱이 같은 형식의 구간 JSON을 쓴다.
- 구간: `gross_J` = ∫V·I dt, `idle_W` = 바로 앞 같은 조건 idle 30초 평균, `net_J = gross_J − idle_W × 구간 길이`, `net J/회 = net_J / 반복 수`.

### 2.2 검증 (단위·부호·갱신 주기) — `results/power/<기기>/calibration.json`

| | C55 | Mix 4 |
|---|---|---|
| 단위 | µA (화면 꺼짐 idle 중앙값 −40 283 → 0.22 W, 4.3 V) | µA (34 000 → 0.30 W, 8.6 V; 1 mA 단위 양자화) |
| 방전 부호 | **음수** | **양수** |
| 연료게이지 실제 갱신 주기 (값이 바뀌는 간격) | 중앙값 250 ms (p10–p90 200–1000 ms) | 중앙값 250 ms (p10–p90 246–498 ms) |
| 샘플 간격 | 50.0 ms | 50.0 ms (구멍 0개, 아래 2.4 참고) |
| 화면 밝기로 방향 확인 | 화면 꺼짐 0.22 W < 밝기 최소 0.68 W < 최대 1.05 W | 앱 창 밝기 0.0/1.0 을 15초씩 6번 교대: 최대가 인접 최소보다 **+0.060–0.075 W** (5/5) |
| 쿨롱 카운터(CHARGE_COUNTER) 교차 확인 | 30초 구간에서는 해상도가 거칠어 쓸 수 없음 (같은 구간 74 J vs 37–66 J) | 동일 |

- 갱신 주기 250 ms ≪ 측정 구간 30초 → 모든 실험은 **같은 작업을 30초 이상 반복**한 구간으로 잰다 (단일 2 ms 추론 같은 짧은 작업은 직접 못 잼).
- 전압은 방송 때만 갱신(수 분 간격)이라 부하 중 전압 강하를 반영하지 못한다 → 부하 구간 에너지가 **추정 1–2% 과대**(4.3 V 중 수십 mV). idle 차분으로 일부 상쇄.

### 2.3 측정 원칙 (두 기기 공통)
- **USB 케이블을 뺀다.** 꽂혀 있으면 충전 전류가 섞인다 (`dumpsys battery unplug`는 표시만 바꾼다). 무선 adb(`adb tcpip` → `adb connect`)로 조작하고,
  실험 시퀀스는 기기 안의 셸 러너가 nohup으로 실행한다 — 구간 중 adb·Wi-Fi 트래픽이 섞이지 않게. PC는 1분마다 상태 파일만 본다. 모든 구간 `plugged=0` 확인.
- 배터리 30–90% (구간 기록에 잔량), 비행기 모드 + adb용 Wi-Fi만, 블루투스 끔.
- cli 실험(E1·E2)은 화면 꺼짐, 앱 실험(E3·E4)은 화면 켜짐 + 창 밝기 0.4 고정 + 카메라 프리뷰 켜짐.
- 구간마다 같은 조건 idle 기준선 30초. 쿨다운: CPU 최고 온도가 시작 온도 +1 °C 안으로 (최대 10–15분). 시작 전 사전 냉각(1분에 0.5 °C 넘게 안 떨어질 때까지).
- 3회 반복, 순서는 번갈아(A B C A B C …; 앱은 홀수 회차 정방향·짝수 회차 역방향), 표는 중앙값(최소–최대).
- 클럭: 구간 중 200 ms마다 cpu0/4/7 `scaling_cur_freq`(평균)와 `scaling_max_freq`(최소 = 스로틀 상한).

### 2.4 무인 측정에서 부딪힌 것 (두 기기)
- **MIUI 백그라운드 제한**: 앱 기본값이 `RUN_ANY_IN_BACKGROUND=ignore` → `Service.startForeground() not allowed due to bg restriction` →
  서비스가 포그라운드가 못 되고 15초 뒤 동결, 샘플 363초 구멍 + 기기가 suspend로 가서 60초 idle 구간이 174초로 늘어남.
  `cmd appops set dev.burstpipe RUN_ANY_IN_BACKGROUND allow` + 절전 예외(`dumpsys deviceidle whitelist +`)로 해결. 이후 구멍 0개.
- **MIUI 스와이프 잠금**: `input keyevent WAKEUP` 뒤 keyguard가 떠 있으면 `activityTimeoutWM=10000` → 10초 만에 화면 꺼짐(설정 timeout 10분과 무관).
  `wm dismiss-keyguard`(스와이프와 같음, 설정 변경 없음) 후 앱의 `FLAG_KEEP_SCREEN_ON`이 유지.
- `am force-stop`은 같은 패키지의 샘플러 서비스도 죽인다 → 러너가 force-stop 뒤에 샘플러를 시작하고, 끝에 샘플러를 먼저 멈춘다. 서비스는 5초마다 파일에 쓴다.
- Windows PC에서 Mix 4는 libusbK 드라이버("Pixel Tablet")로 잡혀 `adb devices`에 안 보임 → `ADB_LIBUSB=1`.

## 3. 결과 — Galaxy C55 [C55]

조건: 케이블 분리, 비행기 모드 + Wi-Fi, 잔량 88→40% (E1 88–87, E2 87–85, E4 69–54, E3 52–40), 실내 23 °C 전후.
샘플러: 앱 `PowerService` (BatteryManager, 50 ms). 입력: `bursts/device/test0` (C55 RAW 4080×3072 × 8).

### E1. 세그멘테이션 백엔드 — 추론 1회당, 기기 전체(배터리) 기준
cli `--seg-only` (256² 입력은 구간 밖에서 1회 준비, 추론만 30초 이상 반복), 화면 꺼짐. 모델: `selfie_segmenter_std.tflite` (float16, 246 노드).

| 백엔드 | **net mJ/회** | net W | ms/회 | 평균 W (gross) | idle W | CPU °C 시작→끝 |
|---|---|---|---|---|---|---|
| **NPU** (QNN HTP v69, fp16, 246/246) | **4.43** (4.31–4.52) | 2.24 | **1.99** | 2.47 | 0.23 | 36→46 |
| GPU (OpenCL, 246/246) | 8.31 (8.07–8.83) | **0.52** | 16.0 | 0.75 | 0.20 | 36→34 |
| CPU (XNNPACK 4스레드) | 40.9 (40.2–41.6) | 4.42 | 9.22 | 4.68 | 0.22 | 39→66 |

- NPU는 GPU보다 **1.9배 적은 에너지로 8배 빠르다**. 평균 전력은 NPU(2.24 W)가 GPU(0.52 W)보다 4배 높지만 1회 시간이 1/8이라 회당 에너지는 절반.
  → "전력(W)이 낮은 쪽"과 "에너지(J)가 적은 쪽"이 다르다. 작업 단위가 정해져 있으면 비교 기준은 J/회.
- CPU는 9 ms로 GPU보다 빠르지만 회당 41 mJ (NPU의 9.2배) + 30초에 CPU 66 °C. 같은 큰 코어를 합성과 나눠 쓰면 합성도 느려진다(기존 측정 4장).
- (참고) 기존 측정의 CPU 370 ms는 MediaPipe 커스텀 op 모델 + 자체 참조 커널 기준. 표준 `TRANSPOSE_CONV`로 바꾼 모델은 XNNPACK이 전체를 가져가 9.2 ms.

### E2. 합성(align+merge+finish) 코어 배치 — burst 1회당, 기기 전체(배터리) 기준
cli, 화면 꺼짐, 세그·보케 없음, 4080×3072 × 8장. C55 코어: 0–3 A510 / 4–6 A710 / 7 A710(최고 클럭).

| 배치 | **net J/burst** | ms/burst | net W | cpu4·cpu7 평균 / 상한 MHz | CPU °C |
|---|---|---|---|---|---|
| scalar 1스레드 (cpu7) | 2.61 (2.56–2.74) | 1519 | 1.71 | –· 2400 / 2400 | 43→48 |
| NEON 리틀 4 (A510, cpu0–3) | 2.19 (2.06–2.23) | 1723 | 1.25 | (cpu0 1804) | 37→42 |
| NEON cpu7 단독 (A710) | 1.81 (1.72–1.84) | 932 | 1.94 | –· 2400 / 2400 | 42→50 |
| **NEON 빅 4 (A710, cpu4–7)** | **1.67** (1.66–1.68) | 318 | 5.24 | 2157·2126 / **2054·2169** | 53→**71** |
| NEON 미고정 8스레드 | 1.72 (1.68–1.74) | **295** | 5.84 | 2124·2044 / **1651·1766** | 55→67 |

- **race-to-idle이 이긴다.** 빅 4코어는 리틀 4코어보다 5.4배 빠르고 에너지는 24% 적다(1.67 vs 2.19 J). 리틀 코어는 전력이 낮지만(1.25 W) 1.7초 동안
  DRAM·L3·SoC 공통 전력이 계속 흐르므로 J/burst가 커진다. 이 작업은 메모리 대역폭을 많이 쓰는(합성 IPC 2.6, LLC miss 4200만, 기존 측정) 형태라
  "효율 코어로 오래"가 불리하다.
- NEON vs scalar(같은 cpu7 1스레드): 932 vs 1519 ms, 1.81 vs 2.61 J → **SIMD가 시간 39%, 에너지 31%를 줄인다.**
- 미고정 8스레드는 빅 4보다 23 ms 빠르지만 리틀 코어까지 깨워 0.05 J 더 쓴다. 30초 안에 클럭 상한이 1651/1766 MHz까지 내려간다(빅 4는 2054/2169) —
  지속 부하에서는 열 예산을 리틀 코어와 나눠 써서 큰 코어가 더 깎인다.
- 주의: 빅 코어 구간은 30초 안에 첫 단계 스로틀(2361→2054 MHz)에 들어간다. 표의 J/burst는 "30초 연속 처리" 조건이고, 단발 셔터(식은 상태)는 더 빠르고 적게 쓸 것이다(추정).

### E4. 앱 단일 촬영의 단계별 에너지 — 기기 전체(배터리) 기준, 카메라 프리뷰 켠 상태 대비
앱(Kotlin Camera2 + JNI), 화면 켜짐·창 밝기 0.4, 세그 NPU. 단계마다 같은 단계를 30초 이상 반복하고 바로 앞 "프리뷰만 켠 30초"를 뺀다.

| 단계 | **net J/회** | ms/회 | net W | 비고 |
|---|---|---|---|---|
| 캡처 (RAW 8장 버스트, 처리 없음) | 0.10 (0.05–0.11) | 518 | 0.14 | 프리뷰가 이미 센서를 돌리고 있어 추가분이 작다 |
| 합성+마무리 (plain) | 1.21 (1.19–1.36) | 475 | 2.55 | |
| 합성+세그+보케 (portrait) | 1.53 (1.45–1.66) | 578 | 2.64 | |
| 세그만 (전처리+NPU 추론) | 0.008 | 7.3 | 1.14 | |
| JPEG (Bitmap + JPEG 95 파일 쓰기) | 0.18 (0.17–0.21) | 199 | 0.97 | |
| 보케+마스크 정제 | **0.31 (추정)** | — | — | = portrait − plain − 세그 (차분) |
| **프리뷰 유지 (idle)** | **2.76 W** | — | — | 화면 + 센서 스트리밍 + ISP. 셔터→JPEG 1.2초 동안 **≈3.3 J** |

- 인물모드 1장 = 캡처 0.10 + 처리 1.53 + JPEG 0.18 ≈ **1.8 J (프리뷰 제외 순 에너지)**. 같은 시간 프리뷰가 ≈3.3 J를 쓴다 →
  **촬영 1장의 기기 전체 에너지에서 처리보다 프리뷰 유지가 더 크다.** 처리 최적화(예: merge 10% 단축 → 0.12 J)보다
  처리 중 프리뷰 프레임레이트·해상도를 낮추는 쪽이 장당 절감이 클 수 있다(추정, 미측정).
- 세그 NPU는 장당 8 mJ로 portrait−plain 차이(0.31 J)의 3%. 인물모드 추가 비용의 대부분은 보케(마스크 정제·블러·합성, CPU)다.

### E3. 150장 연속 촬영 — 기기 전체(배터리) 기준, 셔터→처리→JPEG, 세그·보케 끔
매 회 화면을 끄고 화면-꺼짐 기준 온도(34 °C)+1 °C까지(최대 10분) 식힌 뒤 시작. 정책 off = 고정 N=8, thermal = 지연 피드백 거버너(목표 450 ms). 배터리·시간 때문에 **2회 반복**(원칙 3회에서 줄임).

| 정책 | 150장 총 net J | **net J/장** | 셔터→JPEG ms/장 | 구간 s | 평균 W | 프리뷰 idle W |
|---|---|---|---|---|---|---|
| off (N=8 고정) | 320 (303–337) | 2.13 (2.02–2.25) | 1282 | 200 | 4.37 | 2.77 |
| thermal (거버너) | 316 (308–324) | 2.11 (2.05–2.16) | 1277 | 199 | 4.45 | 2.87 |

30장 단위 (2회 중 r1; `results/power/c55/raw/E3_1027_2140/e3_detail.json`):

| 장 | off 처리 ms | off 셔터→JPEG | thermal 처리 ms | thermal 셔터→JPEG | thermal 평균 N |
|---|---|---|---|---|---|
| 1–30 | 430 | 1203 | 411 | 1187 | 8 |
| 61–90 | 446 | 1267 | 435 | 1254 | 8 |
| 91–120 | 491 | 1323 | 478 | 1328 | 8 |
| 121–150 | **524** | **1368** | **483** | **1330** | **7.5** |

- 스로틀은 두 단계: 큰 코어 상한 2361→2054 MHz가 **6–8장(7–9초)** 에, 1555 MHz까지 깊은 단계가 **68–79장(86–99초)** 에 온다
  (기존 측정 "81장부터 +28%"와 일치). CPU 최고 66–70 °C.
- 거버너는 처리 시간 EMA가 목표 450 ms × 1.1을 넘는 121장 이후에야 N을 8→7(평균 7.3–7.5)로 낮춰, 마지막 30장 처리 시간을 **−41 ms(−8%)** 줄였다.
  150장 전체의 장당 에너지 차이는 **−1%(2.13→2.11 J)로 반복 편차(±0.1 J) 안** — 이 길이에서는 에너지 절감 효과를 주장할 수 없다.
  거버너의 이득은 지연 상한 유지(QoS)이고, 에너지 효과는 스로틀 구간이 더 긴(수백 장) 사용에서 다시 재야 한다.
- 장당 gross(프리뷰 포함) 5.6–6.1 J 중 프리뷰가 ≈3.5 J (2.77 W × 1.28 s). E4와 같은 결론.

## 4. 결과 — Xiaomi Mi Mix 4 [Mix4]

조건: 케이블 분리, 비행기 모드 + Wi-Fi, 잔량 90→70%대, 실내. E1·E2 입력은 **C55에서 덤프한 같은 RAW 버스트**(`test0`, 4080×3072×8)를 push — 같은 입력으로 두 SoC 비교.
샘플러: E1 r1–r2는 앱 `PowerService`, 그 뒤(E1 r3 GPU/CPU, E2, E3, E4)는 **shell 샘플러**(`ShellSampler`, 6장 참고). 같은 구간 동시 기록 비교: idle·GPU 구간 차이 0.1–0.3%,
CPU 전부하 구간은 shell 쪽이 2.3% 낮음(실시간 전압 강하 8.07→7.87 V 반영, 앱 쪽은 방송 전압 고정) — 2.2절의 "부하 구간 1–2% 과대(추정)"를 실측으로 확인.
**MIUI가 큰 코어 상한을 실험 내내 cpu4–6 2112 MHz, cpu7(X1) 2150 MHz로 묶어 둔다** (사양 2419 / 2995 MHz, `scaling_max_freq`). 즉 X1은 2.15 GHz로 돈 결과다.

### E1. 세그멘테이션 백엔드 — 추론 1회당, 기기 전체(배터리) 기준

| 백엔드 | **net mJ/회** | net W | ms/회 | 평균 W | idle W | CPU °C |
|---|---|---|---|---|---|---|
| NPU 요청 → **실제 CPU 폴백** (HTP v68은 fp16 미지원, 246개 연산 전부 검증 실패) | 33.6 (33.5–34.5) | 4.46 | 7.57 | 4.76 | 0.30 | 33→57 |
| **GPU** (OpenCL Adreno 660, 246/246) | **7.55** (7.43–7.62) | 0.46 | 16.5 | 0.75 | 0.29 | 33→30 |
| CPU (XNNPACK 4스레드) | 33.3 (32.8–33.5) | 4.36 | 7.62 | 4.66 | 0.30 | 33→58 |

- "NPU"의 J/회·ms가 CPU와 같다 → 에너지로도 폴백이 확인된다. 델리게이트 생성은 성공("ok")하므로 **로그를 안 보면 NPU로 도는 줄 안다** (6장 1번).
- Mix 4에서 가장 효율적인 경로는 GPU(7.55 mJ). C55 GPU(8.31 mJ)와 비슷하고, C55 NPU(4.43 mJ)보다 1.7배 많다.

### E2. 합성 코어 배치 — burst 1회당, 기기 전체(배터리) 기준
Mix 4 코어: 0–3 A55 1.80 GHz / 4–6 A78 (상한 2.11 GHz) / 7 X1 (상한 2.15 GHz).

| 배치 | **net J/burst** | ms/burst | net W | CPU °C | 반복 |
|---|---|---|---|---|---|
| scalar 1스레드 (X1) | 2.29 (2.26–2.30) | 1426 | 1.61 | 36→41 | 3 |
| NEON 리틀 4 (A55) | 1.68 (1.67–1.70) | 2104 | 0.80 | 31→34 | 3 |
| NEON 미드 3 (A78) | 1.61 (1.61–1.61) | 314 | 5.12 | 43→63 | 2* |
| **NEON X1 단독** | **1.52** (1.52–1.52) | 795 | 1.91 | 37→43 | 2* |
| NEON 미드+X1 4 | 1.62 (1.62–1.64) | 258 | 6.28 | 42→71 | 3 |
| NEON 미고정 8 | 1.61 (1.61–1.64) | **246** | 6.55 | 46→71 | 3 |

\* r3 두 구간은 idle 기준선이 2.3–3.0 W(평소 0.30 W)로 배경 활동(APK 재설치 뒤 MIUI 백그라운드 작업 추정)에 오염 → 분석 규칙(idle > 같은 분석 idle 중앙값×2)으로 제외.

- **C55와 결론이 다르다.** Mix 4는 NEON 배치 사이 차이가 1.52–1.68 J(±5%)로 작고, 가장 적은 것은 **X1 단독**(빠른 코어 1개로 0.8초).
  리틀(A55) 4코어는 6.7배 느린데도 J는 10%만 많다. C55에서는 리틀(A510)이 빅보다 31% 더 썼다.
  → "빨리 끝내고 쉬기 vs 효율 코어로 오래"의 답은 SoC(코어 미세구조·공정·L3/DRAM 공통 전력)마다 다르고, **같은 코드도 기기별로 재야 한다.**
- 미드 3·미드+X1 4·미고정 8은 시간이 1/3인 대신 6–6.5 W로 70 °C까지 오른다 — 30초 연속 부하에서 에너지는 X1 단독과 6% 차이지만 발열 여유는 크게 다르다.
- scalar → NEON (X1 1스레드): 1426 → 795 ms (−44%), 2.29 → 1.52 J (−34%).

### E4. 앱 단일 촬영의 단계별 에너지 — 기기 전체(배터리) 기준, 카메라 프리뷰 켠 상태 대비
Mix 4 자체 촬영(27MP RAW → 가운데 4080×3072), 세그 **GPU**(NPU 불가), 창 밝기 0.4, 잔량 66→44%.

| 단계 | **net J/회** | ms/회 | net W | C55 (참고) |
|---|---|---|---|---|
| 캡처 (RAW 8장) | **0.58** (0.58–0.59) | 499 | 1.16 | 0.10 J / 518 ms |
| 합성+마무리 (plain) | 1.23 (1.21–1.24) | 364 | 3.40 | 1.21 J / 475 ms |
| 합성+세그+보케 (portrait) | 1.51 (1.49–1.53) | 460 | 3.30 | 1.53 J / 578 ms |
| 세그만 (전처리+GPU) | 0.009 | 9.6 | 0.91 | 0.008 J (NPU) / 7.3 ms |
| JPEG | 0.18 (0.17–0.19) | 155 | 1.18 | 0.18 J / 199 ms |
| 보케+마스크 정제 | **0.27 (추정, 차분)** | — | — | 0.31 J |
| **프리뷰 유지 (idle)** | **3.14 W** | — | — | 2.76 W |

- 캡처가 C55의 **6배**(0.58 vs 0.10 J). Mix 4는 RAW가 27MP뿐이라 센서가 매 프레임 27MP를 판독·전송하고, 앱이 행 단위로 잘라 Java 버퍼에 복사한다(포팅 2번).
  센서에서 바로 12MP 모드를 받을 수 없는 한 "자르기"는 메모리 문제만 풀고 에너지는 못 줄인다.
- 처리(plain/portrait)는 두 기기 J가 거의 같고(1.21–1.23 / 1.51–1.53 J) Mix 4가 23% 빠르다 — 빠른 대신 순 전력이 3.3–3.4 W로 높다(C55 2.55–2.64 W).
- 프리뷰가 3.14 W라 Mix 4도 촬영 1장(셔터→JPEG 약 1.0–1.5 s)에서 프리뷰 에너지(≈3–4.5 J)가 처리(≈2.3 J: 캡처 0.58 + portrait 1.51 + JPEG 0.18)보다 크다.

### E3. 150장 연속 촬영 — 기기 전체(배터리) 기준, 셔터→처리→JPEG, 세그·보케 끔
C55와 같은 절차(매 회 화면 꺼짐 기준 34 °C + 1 °C까지 식힘, 2회 반복). 잔량 42→**25%** — 마지막 1회(r2 off)는 원칙 하한 30% 아래에서 끝났다.

| 정책 | 150장 총 net J | **net J/장** | 셔터→JPEG ms/장 | 처리 ms (1–30장 → 121–150장) | 평균 N | 프리뷰 idle W |
|---|---|---|---|---|---|---|
| off (N=8 고정) | 350 (344–356) | 2.33 (2.30–2.37) | 1282 | 387 → 387–396 | 8 | 3.19 |
| thermal (거버너) | 351 (344–357) | 2.34 (2.29–2.38) | 1277 | 386 → 389–391 | 8 | 3.15 |

- **처리 시간이 150장 내내 평평하다(387 ms).** 카메라 앱이 켜져 있는 동안 MIUI가 큰 코어·X1 상한을 **처음부터 1555 MHz로 묶는다**
  (모니터 기록: 시작 29초 전에 이미 cap4·cap7 = 1555 MHz; cli 실험 중에는 2112/2150). 열이 쌓여도 더 내려갈 여지가 없어 스로틀 단계 변화가 없고,
  거버너는 목표(450 ms)를 넘지 않아 N을 한 번도 바꾸지 않았다 → 두 정책이 같은 실험이 됐다(차이 +0.4%, 편차 안).
- C55는 "빠르게 시작 → 86–99초에 깊은 스로틀 → 처리 +22%"인 반면 Mix 4는 "처음부터 느린 상한으로 일정". 같은 앱이라도 **열 정책이 OS 쪽에서 미리 걸려 있으면**
  앱 단 거버너가 할 일이 없다. 사용자 체감(셔터→JPEG 1.28 s)은 두 기기가 같다.


## 5. 두 기기 비교 — 기기 전체(배터리) 기준

> **SoC만 다른 게 아니다**: OS(One UI/Android 16 vs HyperOS/Android 14), 배터리(1셀 4.3 V vs 2셀 8.6 V), 화면 패널, 카메라 센서(50MP vs 108MP, 처리는 둘 다 4080×3072),
> 커널 거버너·열 정책(Mix 4는 큰 코어 상한 2.11/2.15 GHz로 고정)까지 다르다. 아래 차이를 "Snapdragon 7 Gen 1 vs 888+" 차이로만 읽으면 안 된다.
> E1·E2는 같은 입력(C55 RAW `test0`), 화면 꺼짐, idle 차분이라 비교 가능한 편이고, E3·E4는 각 기기 자체 촬영이다.

| 항목 (net, 중앙값) | Galaxy C55 (SM7450) | Mi Mix 4 (SM8350) |
|---|---|---|
| 화면 꺼짐 idle | 0.22 W | 0.30 W |
| E1 세그 NPU | **4.43 mJ / 1.99 ms** | 불가 (v68 fp16 미지원 → CPU 폴백 33.6 mJ) |
| E1 세그 GPU | 8.31 mJ / 16.0 ms | **7.55 mJ** / 16.5 ms |
| E1 세그 CPU 4스레드 | 40.9 mJ / 9.22 ms | 33.3 mJ / 7.62 ms |
| E1 가장 효율적 경로 | NPU 4.43 mJ | GPU 7.55 mJ (1.7배) |
| E2 scalar 1스레드 (최고 클럭 코어) | 2.61 J / 1519 ms | 2.29 J / 1426 ms |
| E2 NEON 리틀 4 | 2.19 J / 1723 ms | 1.68 J / 2104 ms |
| E2 NEON 최고 클럭 코어 1개 | 1.81 J / 932 ms (A710 2.40 GHz) | **1.52 J** / 795 ms (X1 2.15 GHz) |
| E2 NEON 큰 코어 4 | **1.67 J** / 318 ms | 1.62 J / 258 ms (A78×3 + X1) |
| E2 NEON 미고정 8 | 1.72 J / 295 ms | 1.61 J / 246 ms |
| E2 최소 에너지 배치 | 빅 4 (race-to-idle) | X1 단독 (배치 간 차이 ±5%) |
| E2 리틀 vs 최소 | +31% | +10% |
| E4 프리뷰 유지 (화면+센서+ISP) | 2.76 W | 3.14 W |
| E4 캡처 RAW 8장 | 0.10 J | **0.58 J** (27MP 판독 + 자르기) |
| E4 합성+마무리 / 인물모드 처리 | 1.21 / 1.53 J (475 / 578 ms) | 1.23 / 1.51 J (364 / 460 ms) |
| E4 세그 1회 (앱, 전처리 포함) | 8 mJ (NPU) | 9 mJ (GPU) |
| E4 JPEG | 0.18 J | 0.18 J |
| E3 150연속 net J/장 (off / 거버너) | 2.13 / 2.11 J | 2.33 / 2.34 J |
| E3 셔터→JPEG (2회 중앙값, 우연히 일치 — 회차값 C55 1262·1302 / Mix4 1249·1315) | 1282 / 1277 ms | 1282 / 1277 ms |
| E3 스로틀 | 7–9 s 1단계(2361→2054), 86–99 s 2단계(→1555 MHz), 처리 430→524 ms | 카메라 중 처음부터 1555 MHz 고정, 처리 387 ms 일정 |

## 6. Mi Mix 4 포팅 — 생긴 문제와 해결

| # | 문제 | 원인 | 해결 / 상태 |
|---|---|---|---|
| 1 | **NPU가 조용히 CPU로 돈다** (델리게이트 생성 "ok", 추론 7.5 ms) | Hexagon 780 = HTP **v68은 fp16 미지원** (fp16 HTP는 v69+). float16 모델의 246개 연산 전부 `QnnBackend_validateOpConfig failed 0xc26` → TFLite가 전부 CPU로 실행 | `fetch_qnn.sh`가 v68·v69 Stub/Skel을 모두 패키징, `seg_litert.cc`가 `ro.soc.model`로 HTP 버전을 로그(SM8350→v68). INT8 시도: `tools/quantize_selfie_int8.py`(fp16 DEQUANTIZE 110개를 fp32 상수로 접고 SignatureDef 추가 → ai-edge-quantizer). a16w8 모델은 PC LiteRT 2.3에서 float 대비 마스크 IoU 0.96(실사 인물)·0.99(에뮬), 그러나 **기기 LiteRT 2.16이 로드 실패**("Input tensor lacks data": 양자화기가 연산 버전을 v1로 둔 채 int16 활성/int8 가중치 conv를 만듦). `seg_litert.cc`는 int8/int16 입출력·`kHtpQuantized`를 지원하게 고쳐 둠. **미해결** — 기기 런타임을 새 LiteRT로 올리거나 QNN 컨버터로 v68 컨텍스트 바이너리를 만들어야 한다. Mix 4 앱은 GPU 세그 사용 |
| 2 | 앱 촬영 직후 **OutOfMemoryError** | 메인 카메라 RAW_SENSOR가 **6016×4512(27MP) 하나뿐** (108MP 센서 비닝). 8장 슬롯 434MB + 출력 버퍼가 Java 힙 512MB 초과 (Android의 `allocateDirect`는 Java 힙) | RAW가 16MP를 넘으면 가운데 **4080×3072로 잘라** 슬롯에 복사(오프셋 짝수 → CFA 위상 유지, `meta.txt`에 `crop`). C55와 픽셀 수가 같아 장당 에너지 비교도 같은 조건. 셔터→JPEG 1.50 s (캡처 806 ms: 27MP 판독) |
| 3 | 연속 캡처(처리 없이)만 하면 OOM | 셔터마다 슬롯 8×25MB를 새로 할당. 평소에는 처리 400 ms가 GC 시간을 벌어줘 우연히 통과 (C55에서도 재현) | 연속 캡처 경로는 이전 슬롯 재사용 (`CaptureController.lastSlots`) |
| 4 | 측정 앱이 7–16분 뒤 죽음 | **MIUI PowerKeeper `AutoPowerKill`**: 화면 꺼진 뒤 포그라운드 서비스도 force-stop (`dumpsys activity exit-info`: "stop dev.burstpipe due to AutoPowerKill"). 앱 배터리 설정 "제한 없음"을 켜도 재발 | 샘플러를 **shell uid 프로세스로** (`app_process` + `ShellSampler`): `IBatteryPropertiesRegistrar.getProperty(CURRENT_NOW)`(= BatteryManager와 같은 값) + shell이 읽을 수 있는 `vbat` thermal zone(실시간 전압 × 셀 수 2), `IPowerManager`로 shell PARTIAL wake lock. 앱 샘플러와 0.1–0.3% 일치 |
| 5 | 백그라운드 서비스가 포그라운드가 못 됨 | MIUI 기본 `RUN_ANY_IN_BACKGROUND=ignore` → "startForeground() not allowed due to bg restriction" | `cmd appops set … RUN_ANY_IN_BACKGROUND allow` + 절전 예외 |
| 6 | 깨운 화면이 10초 만에 꺼짐 | 스와이프 keyguard가 떠 있으면 `activityTimeoutWM=10000` | `wm dismiss-keyguard` |
| 7 | adb·설치·입력 | 개발자 옵션 "USB를 통해 설치", "USB 디버깅(보안 설정)" 필요. Windows에서는 libusbK 드라이버라 `ADB_LIBUSB=1` | 사용자 설정 + 래퍼 |

C55에서 찾았던 기기 전용 문제 6개가 Mix 4에서도 나오는가:

| C55 문제 | Mix 4 |
|---|---|
| 결과 CCM이 단위행렬 | **재현** — `ccm_source forward_matrix`로 ForwardMatrix(D65) CCM 사용 (기존 코드가 그대로 처리) |
| 노이즈 프로파일 | 타당한 값 (a=2.4e-3, b=1.8e-5 @ ISO 1600) → HAL 값 사용. AVD처럼 엉터리는 아님 |
| 센서 방향 | 90° (C55와 같음), 기존 정립 회전으로 처리. CFA는 다름: Mix 4 **GRBG**, C55 GBRG — 메타로 처리 |
| GPU 델리게이트 스레드 친화성 | 재현 없음 (전용 소유 스레드가 이미 있음). 앱 GPU 세그 init 1.7 s, 추론 8.6 ms |
| 벤더 라이브러리 네임스페이스 (`uses-native-library`) | 같은 매니페스트로 동작 (`uses_libraries=libOpenCL.so:libcdsprpc.so` 로드 로그) |
| perf 제약 (삼성 user 빌드: 커널 샘플·소프트웨어 이벤트 금지) | **없음** — `perf_event_paranoid=-1`, `cpu-cycles:k`·`task-clock` 모두 허용 |

추가로 Mix 4는 ISO 상한이 1600(C55 6400)이라 저조도 버스트가 어두워진다(노출 정책은 그대로). 앱 GPU 세그·보케·JPEG까지 끝까지 동작 확인.

## 7. 한계
- **기기 전체(배터리) 전력**이다. 레일별(SoC·DDR·카메라) 분해가 없어 "NPU 칩 전력"이 아니라 "NPU로 돌릴 때 기기 전체가 더 쓰는 전력"이다. idle 차분이 화면·모뎀·상주 서비스를 지우지만 DRAM·L3·전원 레일 효율(PMIC)은 작업 쪽에 포함된다.
- 연료게이지 갱신 250 ms, BatteryManager 전류는 게이지 내부 평균값 → **30초 이상 반복한 평균**만 의미가 있다. 2 ms 추론 1회, 셔터 1회 같은 단발 사건은 직접 못 재고 반복 평균으로 나눈 값이다.
- 전압: C55·Mix 4 E1(r1–r2)의 앱 샘플러는 방송 전압(수 분 간격 갱신)이라 전부하 구간 에너지가 **+2.3% 과대**(Mix 4 동시 측정). Mix 4 나머지는 실시간 `vbat`. 두 기기 비교에서 수 % 차이는 해석하지 않는다.
- 30초 연속 부하는 **스로틀을 포함**한다(C55 빅 코어 30초 안에 2361→2054 MHz). 식은 상태의 단발 셔터 에너지는 표보다 작을 것이다(추정).
- 카메라 프리뷰가 켜진 앱 실험에서 CPU 최고 온도 센서가 1초 안에 수 °C 흔들려 "시작 +1 °C" 쿨다운 기준이 사실상 상한(2–15분)으로 동작했다. 순서 번갈아·직전 idle 차분으로 편향을 줄였지만 단계 간 온도는 같지 않다.
- 반복: E1·E2·E4 3회, **E3 2회**(배터리·시간 때문에 줄임). Mix 4 E2 미드3·X1 단독은 r3가 배경 활동으로 제외돼 2회.
- 잔량: C55 E1이 88%에서 시작(원칙 30–90% 안), 실험 순서상 E3가 가장 낮은 잔량(C55 52→40%, Mix 4 42→**25%**)에서 돌았다 — Mix 4 E3 마지막 1회(r2 off)는 원칙 하한 30% 아래. 잔량에 따른 전압 차이(4.3→3.9 V)는 P=V×I로 반영되지만 게이지 오차는 잔량 구간마다 다를 수 있다.
- Mix 4 큰 코어 상한이 MIUI에 의해 2.11/2.15 GHz로 묶인 상태의 결과다. 성능 모드에서는 다를 수 있다(미측정).
- C55 E3의 거버너 효과(−1%)는 반복 편차 안이다. "거버너가 에너지를 줄인다"는 주장은 이 데이터로 못 한다.

## 8. 재현

```bash
# 빌드 (cli NEON / scalar, 앱)
NDK=... bash scripts/build_arm64.sh && NO_NEON=1 NDK=... bash scripts/build_arm64.sh
QAIRT_SDK=... bash scripts/fetch_qnn.sh          # v68 + v69 Stub/Skel
cd android && ./gradlew assembleDebug && cd ..
# 케이블 연결 상태에서 무선 adb 준비 → 케이블 분리
adb tcpip 5555 && adb connect <phone-ip>:5555
# 샘플러 검증 (단위·부호·갱신 주기) → results/power/<dev>/calibration.json
python tools/power_analyze.py validate --csv samples.csv --records <val dir> --out results/power/<dev>/calibration.json
# cli 실험 (E1 세그, E2 코어 배치). MIUI는 SAMPLER=shell
ADB="adb -s <ip>:5555" bash scripts/power_cli.sh c55 E1 3
ADB="adb -s <ip>:5555" SAMPLER=shell bash scripts/power_cli.sh mix4 E2 3
# 앱 실험 (E4 단계별, E3 150연속)
ADB="adb -s <ip>:5555" DELEGATE=npu COOL_MAX=120 bash scripts/power_app.sh c55 E4 3
ADB="adb -s <ip>:5555" DELEGATE=gpu SAMPLER=shell bash scripts/power_app.sh mix4 E3 2
# 표·세부
python tools/power_report.py c55 mix4
python tools/power_e3.py results/power/<dev>/raw/<E3 run> --calib results/power/<dev>/calibration.json
```
cli: `--seconds S`(웜업 1회 뒤 S초 이상 반복), `--seg-only`, `--idle S`, `--power-json w.json --label L`.
앱: `--es action pstage --es stage idle|capture|plain|portrait|seg|jpeg --ei seconds 30 --es label L`, `--es action stress --ei count 150 --es policy off|thermal --ez noseg true --es label L`.

## 업로드용 요약

```
[조건]
C55 : Galaxy C55 SM-C5560, Snapdragon 7 Gen 1 (SM7450, HTP v69), Android 16 / One UI, 1셀 ~4.3 V.
      케이블 분리·비행기 모드+Wi-Fi(무선 adb), 잔량 88→40%, cli는 화면 꺼짐 / 앱은 창 밝기 0.4·프리뷰 켬.
      샘플러: 앱 PowerService (BatteryManager CURRENT_NOW, µA·방전 음수, 게이지 갱신 250 ms, 50 ms 샘플).
Mix4: Xiaomi Mi Mix 4, Snapdragon 888+ (SM8350, HTP v68), Android 14 / HyperOS 1.0, 2셀 ~8.6 V.
      같은 조건, 잔량 90→25%(마지막 E3 1회만 30% 미만). MIUI가 큰 코어 상한을 2.11/2.15 GHz(카메라 중 1.56 GHz)로 고정.
      샘플러: shell ShellSampler (app_process, µA·방전 양수, 실시간 vbat×2셀) — 앱 샘플러와 0.1–0.3% 일치.
모든 수치 = 기기 전체(배터리) 기준, 같은 조건 idle 30초 차분(net), 30초+ 반복 구간, 중앙값(3회; E3 2회).

[E1 세그 추론 1회당 net — selfie_segmenter 256², 같은 입력]
              NPU              GPU              CPU(XNNPACK 4T)
C55        4.43 mJ / 1.99 ms  8.31 mJ / 16.0 ms  40.9 mJ / 9.2 ms
Mix4       불가(v68 fp16 미지원→CPU 폴백 33.6 mJ)  7.55 mJ / 16.5 ms  33.3 mJ / 7.6 ms

[E2 합성 burst 1회(4080×3072×8) net — 같은 C55 RAW 입력]
              scalar 1T   NEON little4   NEON top-core 1   NEON big4      NEON unpinned8
C55        2.61 J      2.19 J/1723ms   1.81 J/932ms      1.67 J/318ms*  1.72 J/295ms
Mix4       2.29 J      1.68 J/2104ms   1.52 J/795ms*     1.62 J/258ms   1.61 J/246ms
(* 최소. C55 = race-to-idle(빅 코어, 리틀 +31%), Mix4 = X1 단독(배치 간 ±5%, 리틀 +10%). NEON은 scalar 대비 에너지 −31/−34%)

[E3 150장 연속 촬영 net J/장 (셔터→JPEG ms), 세그·보케 끔]
              거버너 끔          거버너 켬
C55        2.13 J (1282)      2.11 J (1277)   스로틀 7–9 s 1단계, 86–99 s(68–79장) 2단계 → 처리 430→524 ms, 거버너는 121장부터 N 8→7.3, 마지막 30장 −41 ms
Mix4       2.33 J (1282)      2.34 J (1277)   카메라 중 OS가 처음부터 1.56 GHz 고정 → 처리 387 ms 일정, 거버너 개입 없음
→ 두 기기 모두 정책 간 에너지 차이는 반복 편차 안(±1%).

[E4 인물모드 1장 단계별 net (프리뷰 대비)]  C55: 캡처 0.10 + 처리 1.53 + JPEG 0.18 ≈ 1.8 J, 프리뷰 유지 2.76 W(≈3.3 J/장)
                                         Mix4: 캡처 0.58 + 처리 1.51 + JPEG 0.18 ≈ 2.3 J, 프리뷰 유지 3.14 W
→ 장당 기기 에너지에서 프리뷰 유지가 처리보다 크다.

[Mix 4 포팅 이슈]
1. HTP v68 fp16 미지원 → 246개 연산 전부 검증 실패, 델리게이트는 "ok"인데 CPU로 실행 (로그·에너지로 확인). INT8/a16w8 모델 생성(PC IoU 0.96)했으나 기기 LiteRT 2.16 로드 실패 — 미해결, GPU 사용
2. RAW가 27MP(6016×4512) 하나뿐 → 8장 슬롯이 Java 힙 초과(OOM) → 가운데 4080×3072 자르기 (캡처 에너지는 C55의 6배로 남음)
3. 연속 캡처 시 슬롯 재할당 OOM (C55에서도 재현) → 슬롯 재사용
4. MIUI AutoPowerKill이 화면 꺼진 7–16분 뒤 포그라운드 서비스까지 종료 → shell uid 샘플러(app_process + IBatteryPropertiesRegistrar + IPowerManager wake lock)
5. MIUI 백그라운드 제한(startForeground 거부) → appops RUN_ANY_IN_BACKGROUND / 스와이프 keyguard 10초 타임아웃 → dismiss-keyguard
6. C55 기기 문제 재현 여부: CCM 단위행렬 재현(ForwardMatrix로 처리), 노이즈 프로파일·방향·GPU 스레드·네임스페이스 정상, perf 제약 없음(paranoid −1). CFA GRBG, ISO 상한 1600

[이력서용 한 줄 후보]
- 두 Snapdragon 기기(7 Gen 1 / 888+)에서 배터리 전류 샘플러와 idle 차분 측정 체계를 만들어 RAW 버스트 HDR·인물모드 파이프라인의 단계별 에너지를 실측(세그 추론 NPU 4.4 mJ vs GPU 8.3 mJ vs CPU 41 mJ).
- 합성 코어 배치별 에너지를 재서 race-to-idle이 SoC마다 다름을 확인: 7 Gen 1은 빅 코어가 리틀보다 24% 적게, 888+는 X1 단독이 최소이고 배치 간 차이 ±5%.
- Mi Mix 4 포팅에서 HTP v68 fp16 미지원으로 NPU가 조용히 CPU 폴백(C55 NPU 대비 회당 에너지 7.6배)되는 것, 27MP RAW OOM, MIUI AutoPowerKill로 측정 서비스가 죽는 문제를 찾아 각각 진단·우회(shell 권한 샘플러).
```
