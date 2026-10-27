# 이슈 기록 — 전력 측정 · Mi Mix 4 포팅 (2026-10-27 ~ 28)

`docs/power.md`의 측정을 하면서 생긴 문제를 **증상 → 원인 → 근거 → 해결 → 상태** 순서로 적는다.
분류: **[P] 포팅**(앱·모델이 Mix 4에서 동작하게), **[M] 측정 인프라**(무인 전력 측정이 맞는 값을 내게), **[T] 도구·환경**.
상태: ✅ 해결 / ⚠️ 우회(근본 원인은 남음) / ❌ 미해결.

## 요약

| # | 분류 | 이슈 | 상태 |
|---|---|---|---|
| 1 | P | Mix 4 NPU(HTP v68)가 fp16 모델을 못 받아 조용히 CPU로 실행 | ✅ INT8 모델로 136/136 노드 NPU 위임 |
| 2 | P | INT8 모델이 기기 LiteRT 2.16.1에서 로드 실패 | ✅ LiteRT 1.4.2 런타임으로 로드 (앱은 아직 2.16.1) |
| 3 | P | INT8 양자화 후 장면에 따라 마스크 정확도 급락 | ⚠️ 보정 데이터로 개선, 일부 장면 남음 |
| 4 | P | Mix 4 RAW 27MP → 앱 OOM | ✅ 가운데 4080×3072 자르기 |
| 5 | P | 연속 캡처(처리 없이) OOM — C55에서도 재현 | ✅ 슬롯 재사용 |
| 6 | P | 앱 크래시가 같은 프로세스의 샘플러까지 죽임 | ✅ 샘플러를 `:power` 별도 프로세스로 |
| 7 | M | shell에서 배터리 전류 sysfs 읽기 불가 (두 기기) | ⚠️ 앱/쉘 BatteryManager 경로로 대체 |
| 8 | M | `dumpsys battery`의 current now가 캐시값 | ✅ 사용하지 않음 |
| 9 | M | MIUI 백그라운드 제한으로 포그라운드 서비스 시작 거부 | ✅ appops 허용 |
| 10 | M | MIUI AutoPowerKill이 7–16분 뒤 측정 앱 강제 종료 | ✅ shell uid 샘플러 |
| 11 | M | shell 샘플러에 wake lock이 없어 화면 꺼진 구간에서 suspend | ✅ IPowerManager wake lock |
| 12 | M | 사전 냉각 단계에서 샘플러 시작 전이라 suspend | ✅ 샘플러를 먼저 시작 |
| 13 | M | MIUI 스와이프 잠금 때문에 화면이 10초 만에 꺼짐 | ✅ `wm dismiss-keyguard` |
| 14 | M | `am force-stop`이 샘플러 서비스도 종료 | ✅ 순서 조정 + 5초마다 flush |
| 15 | M | 쿨다운 목표 온도에 도달 못 해 매 구간 상한(10–15분)까지 대기 | ⚠️ 상한 2분으로 단축, 문서에 기록 |
| 16 | M | 배경 활동이 idle 기준선을 오염 (Mix 4 E2 2구간) | ✅ 자동 제외 규칙 |
| 17 | M | 앱 샘플러 전압이 방송값이라 전부하 구간 +2.3% 과대 | ⚠️ 정량화해 문서화 |
| 18 | M | 실험 중 시스템 팝업이 앱을 가림 (삼성 계정 로그인 확인, Android 16 16KB 경고) | ✅ 사용자 처리 후 재측정 |
| 19 | M | 배터리 하한 30% 아래로 내려간 구간 | ⚠️ 문서에 표시 |
| 20 | T | Windows에서 Mix 4가 adb에 안 보임 | ✅ `ADB_LIBUSB=1` |
| 21 | T | Git Bash 경로 변환·heredoc 이스케이프·CRLF | ✅ 작업 방식으로 회피 |

---

## [P] 포팅

### 1. Mix 4 NPU가 fp16 모델을 못 받아 조용히 CPU로 실행 — ✅
- **증상**: `--delegate npu`로 델리게이트 생성은 "ok"이고 추론도 되는데 7.5 ms(C55 NPU 2 ms). 에너지도 CPU와 같은 33.6 mJ/회.
- **원인**: Snapdragon 888+의 Hexagon 780 = **HTP v68은 fp16 연산을 지원하지 않는다**(fp16 HTP는 v69부터). float16 모델의 246개 연산이 전부 QNN 검증에서 탈락하고, TFLite가 탈락한 연산을 CPU로 돌려서 겉으로는 정상처럼 보인다.
- **근거**: logcat `QnnDsp <E> Failed to validate op ... with error 0xc26` 246건(Cast 110, Conv2d 43, Relu 22, …). E1 에너지: "NPU" 33.6 mJ = CPU 33.3 mJ.
- **해결**: `tools/quantize_selfie_int8.py`로 정적 INT8(a8w8) 모델을 만들었다(fp16 DEQUANTIZE 110개를 fp32 상수로 접고 SignatureDef 추가 → ai-edge-quantizer 0.10). `seg_litert.cc`는 정수 입출력 변환과, 파일명에 `int8`이 있으면 `kHtpQuantized`를 쓰도록 했다.
  결과: **136/136 노드, 1 파티션이 HTP v68에 위임**. 측정(기기 전체, net): **3.88 mJ / 4.15 ms**(2회) — fp16 CPU 폴백 33.6 mJ의 1/8.7, Mix 4 GPU 7.55 mJ의 절반.
- **배운 점**: 델리게이트 생성 성공 ≠ NPU 실행. 위임된 노드 수(`Replacing N out of M node(s)`)와 실제 J/회로 확인해야 한다. `seg_litert.cc`가 SoC→HTP 버전을 로그에 남기게 했다.
- 시도했지만 버린 것: **a16w8**(16비트 활성) — v68이 int16 활성 Conv2d·DepthwiseConv·HardSwish·AvgPool·TransposeConv를 지원하지 않아 82/158 노드, 124 파티션, 127 ms.

### 2. INT8 모델이 기기 LiteRT 2.16.1에서 로드 실패 — ✅ (cli), 앱 미적용
- **증상**: CPU·NPU 모두 `ERROR: Input tensor 323 lacks data` → 세그멘터 생성 실패. PC의 ai-edge-litert 2.3에서는 정상 동작.
- **원인**: ai-edge-quantizer 0.10이 만든 양자화 그래프를 기기의 TFLite 2.16.1 런타임이 처리하지 못한다(같은 파일이 최신 런타임에서는 정상). 연산 버전 필드도 v1로 남아 있었다(int16×8 conv인데 CONV_2D v1).
- **근거**: 같은 파일을 **LiteRT 1.4.2 AAR**(Google Maven `com.google.ai.edge.litert:litert:1.4.2`)의 `libtensorflowlite_jni.so`로 바꿔 `LD_LIBRARY_PATH`로 먼저 찾게 하자 재빌드 없이 로드됨(C API 호환). QNN 델리게이트도 그대로 동작.
- **해결**: 측정용 cli는 `LIBDIR=/data/local/tmp/bp/l142`로 1.4.2를 쓴다(`scripts/power_cli.sh`).
- **남은 일**: 앱은 아직 2.16.1(fp16 모델 + GPU/NPU v69). 앱을 1.4.2로 올리면 C55의 기존 측정과 런타임이 달라지므로 이번엔 하지 않았다. 참고: 1.4.2 C API는 CPU 경로에 XNNPACK을 기본 적용하지 않아 CPU 추론이 수백 ms로 느려진다 → 앱 이관 시 XNNPACK 델리게이트를 명시적으로 붙여야 한다.

### 3. INT8 양자화 후 장면에 따라 마스크 정확도 급락 — ⚠️
- **증상**: float 대비 0.5 이진화 IoU가 인물 장면에서 0.83–0.95인데, 사람이 없는 책상 장면(`test0`)에서 화면 전체를 전경으로 판정(평균 |Δα| 0.6)하거나, 작은 전경 장면에서 IoU 0에 가까움.
- **원인**: 디코더의 채널 게이팅 경로(global average pool → 1×1 conv → sigmoid → 곱)가 **per-tensor int8**에 약하다. 보정 데이터 중 일부 장면이 이 경로의 활성 범위를 크게 넓혀(예: `average_pooling2d_9` 실제 −2.7..1.5인데 범위 66.7 → 한 단계 0.26) 다른 장면에서 해상도가 모자란다. MobileNetV3류(SE + hard-swish) 모델의 알려진 per-tensor 양자화 문제.
- **근거**: 층별 비교 스크립트(양쪽 모델의 모든 중간 텐서를 역양자화해 float과 비교)에서 오차가 이 경로에서 처음 커진다.
- **조치**: 보정 데이터를 **실기기 버스트만**(버스트당 frame 0/4/7 + 좌우 반전)으로 바꿈. 실기기 7장면 평균 |Δα|: 0.081 (노출 증강 0.138, 에뮬 포함 0.169). `test0` 오판정은 사라졌다(|Δα| 0.0001).
  남은 문제: 7장면 중 1장면(작은 전경)은 IoU 0.0003, 1장면(밝은 패널) 0.55. 인물 장면 0.86.
- **다음 단계(미실행)**: 실제 사용 분포에 가까운 보정 세트(수백 장), 게이팅 경로만 int16(v68이 지원하는 Mul/Add/Logistic 범위 안), 또는 QAT. `docs/power.md`에서 NPU INT8 수치는 "정확도 차이 있음"과 함께만 쓴다.

### 4. Mix 4 RAW 27MP → 앱 OOM — ✅
- **증상**: 촬영 직후 `java.lang.OutOfMemoryError`(MIUI 로그 `JE_OOM`).
- **원인**: Mix 4 메인 카메라의 `RAW_SENSOR`가 **6016×4512(27MP, 108MP 센서 비닝) 하나뿐**. 8장 슬롯 434MB + 출력 108MB가 Java 힙(512MB)을 넘는다. Android의 `ByteBuffer.allocateDirect`는 Java 힙에서 잡힌다.
- **해결**: RAW가 16MP를 넘으면 가운데 **4080×3072로 잘라** 슬롯에 복사(오프셋 짝수 → CFA 위상 유지, `meta.txt`에 `crop`). C55와 픽셀 수가 같아 장당 에너지 비교 조건도 맞춘다.
- **부작용(측정됨)**: 센서는 여전히 27MP를 판독하므로 캡처 에너지가 C55의 6배(0.58 vs 0.10 J/버스트). 자르기는 메모리 문제만 푼다.

### 5. 연속 캡처(처리 없이) OOM — ✅ (C55에서도 재현)
- **증상**: E4 "캡처만" 단계에서 2번째 버스트 뒤 앱 종료.
- **원인**: 셔터마다 8×25MB 슬롯을 새로 할당. 평소에는 처리 400 ms 동안 GC가 돌아 우연히 통과했다.
- **해결**: 연속 캡처 경로는 직전 슬롯을 재사용(`CaptureController.lastSlots`). 일반 촬영 경로는 기존 측정과 같은 조건을 위해 그대로 둠.

### 6. 앱 크래시가 같은 프로세스의 샘플러까지 죽임 — ✅
- **증상**: C55 E4 첫 시도에서 16분 이후 샘플 0개 → 그 뒤 구간 전부 무효, E4 전체 재측정.
- **원인**: `PowerService`가 앱 메인 프로세스 안에 있었다(5번 OOM으로 함께 종료).
- **해결**: 매니페스트에서 `android:process=":power"`로 분리.

## [M] 측정 인프라

### 7. shell에서 배터리 전류 sysfs 읽기 불가 — ⚠️
- **증상**: `/sys/class/power_supply/*/current_now` 전부 Permission denied(C55·Mix 4 모두, battery/usb/ac/wireless/otg).
- **확인한 대안**: thermal_zone `vbat`(전압)은 읽힘. `*-ibat-lvl*` 영역은 BCL 임계값용이라 값이 일관되지 않음(lvl0 156 / lvl1 468). `service call batteryproperties`는 파싱 실패.
- **해결**: 앱 `BatteryManager.BATTERY_PROPERTY_CURRENT_NOW`(C55·Mix 4 E1 r1–r2) → shell `app_process`에서 같은 바인더(`IBatteryPropertiesRegistrar.getProperty`)를 직접 호출(Mix 4 나머지). cli의 sysfs 샘플러는 남겨 두되 실패 이유를 결과 JSON에 기록.

### 8. `dumpsys battery`의 current now가 캐시값 — ✅
- 5초 간격으로 읽어도 같은 값(−409 551). 샘플링에 쓰지 않고, 밝기 검증의 참고값으로만 기록.

### 9. MIUI 백그라운드 제한으로 포그라운드 서비스 시작 거부 — ✅
- **증상**: Mix 4에서 샘플이 15초 뒤 끊기고 60초 idle 구간이 174초로 늘어남(기기 suspend).
- **원인**: 앱 기본값 `RUN_ANY_IN_BACKGROUND=ignore` → `Service.startForeground() not allowed due to bg restriction` → 서비스가 포그라운드가 못 되고 동결.
- **해결**: `cmd appops set dev.burstpipe RUN_ANY_IN_BACKGROUND allow` + 절전 예외. 러너가 매번 설정.

### 10. MIUI AutoPowerKill이 측정 앱 강제 종료 — ✅
- **증상**: 화면 꺼진 뒤 7–16분이면 샘플이 끊기고 이후 구간 무효(Mix 4 E1·E2 첫 시도).
- **원인**: `dumpsys activity exit-info` → `stop dev.burstpipe due to AutoPowerKill`. 포그라운드 서비스(importance 125)도 대상. 앱 배터리 설정 "제한 없음"을 켠 뒤에도 재발(설정 직후 APK 재설치로 초기화됐을 가능성).
- **해결**: 샘플러를 **shell uid 프로세스**로: `CLASSPATH=$(pm path dev.burstpipe) app_process /system/bin dev.burstpipe.ShellSampler`.
  검증: 같은 구간 동시 기록 시 앱 샘플러와 idle·GPU 구간 0.1–0.3% 일치.

### 11. shell 샘플러에 wake lock이 없어 suspend — ✅
- **증상**: Mix 4 E2 재시도에서 10분 동안 CSV 54KB(정상의 1/10), 반복이 3–14초로 늘어남.
- **원인**: 앱 서비스는 PARTIAL wake lock을 잡았지만 shell 프로세스는 안 잡음. `/sys/power/wake_lock`은 shell 권한 없음.
- **해결**: shell uid에는 `WAKE_LOCK` 권한이 있으므로 `IPowerManager.acquireWakeLock`을 리플렉션으로 호출(`dumpsys power`에 `burstpipe:shell_sampler` 확인).

### 12. 사전 냉각 단계에서 샘플러 시작 전이라 suspend — ✅
- **증상**: Mix 4 E3에서 사전 냉각(최대 5분)이 12분 넘게 진행.
- **원인**: 러너가 화면 끈 상태로 기준 온도를 재는 동안 아직 샘플러(=wake lock)를 시작하지 않았다.
- **해결**: shell 샘플러는 force-stop과 무관하므로 러너 맨 앞에서 시작.

### 13. MIUI 스와이프 잠금 때문에 화면이 10초 만에 꺼짐 — ✅
- **증상**: 앱이 `FLAG_KEEP_SCREEN_ON`인데도 Mix 4 화면이 꺼지고, 밝기 검증 구간이 화면 꺼짐 전력과 같게 나옴.
- **원인**: `PowerManagerService: Going to sleep due to timeout (screenOffTimeout=600000, activityTimeoutWM=10000)` — keyguard가 떠 있으면 10초.
- **해결**: 깨운 뒤 `wm dismiss-keyguard`(스와이프와 같음, 설정 변경 없음).

### 14. `am force-stop`이 샘플러 서비스도 종료 — ✅
- 러너가 앱 재시작용 force-stop을 샘플러 시작 뒤에 했다 → 순서를 바꾸고(force-stop → 샘플러 → 앱), 끝에는 샘플러를 먼저 멈춘다. 서비스는 5초마다 파일에 붙여 쓴다.

### 15. 쿨다운 목표 온도에 도달 못 함 — ⚠️
- **증상**: "시작 온도 +1 °C" 기준이 카메라 켠 상태(C55 E4)나 배경 활동 중(Mix 4 E2)에서 끝내 충족되지 않아 매번 상한(10–15분)까지 대기 → 측정 시간·배터리 낭비.
- **원인**: 카메라 프리뷰 중 CPU 최고 온도 센서가 1초 안에 수 °C 흔들림. 시작 온도를 아주 식은 상태에서 잡음.
- **조치**: 앱 E4는 상한 2분, cli 상한 `COOL_MAX`, 사전 냉각 최대 5분. E3만 화면을 끄고 10분까지 식힘. 순서 번갈아 + 직전 idle 차분으로 편향을 줄였지만 단계 간 온도가 같지는 않다(문서 한계에 기록).

### 16. 배경 활동이 idle 기준선을 오염 — ✅
- **증상**: Mix 4 E2 r3 두 구간의 idle이 2.3–3.0 W(평소 0.30 W).
- **원인(추정)**: 직전 APK 재설치 뒤 MIUI 백그라운드 작업(dex 최적화·보안 스캔). 같은 시간 CPU 온도가 idle인데도 43 °C.
- **해결**: 분석 규칙 — idle_W가 같은 분석의 idle 중앙값×2를 넘으면 그 구간을 요약에서 제외(원본은 보존). 다른 모든 런은 제외 0건.

### 17. 앱 샘플러 전압이 방송값 — ⚠️
- `EXTRA_VOLTAGE`는 방송 때만 갱신(수 분). Mix 4에서 shell 샘플러(실시간 vbat)와 동시 기록: CPU 전부하에서 앱 쪽 +2.3%(8.07→7.87 V 강하 미반영), idle·GPU 0.1–0.3%. C55 수치에는 이 오차가 남아 있다.

### 18. 실험 중 시스템 팝업 — ✅
- C55: 삼성 계정 로그인 확인 팝업이 앱을 가림 → 인증 화면이라 건드리지 않고 사용자에게 요청, 해당 런 폐기 후 재측정.
- C55(Android 16): 디버그 앱에 "16KB 페이지 정렬 아님" 호환성 경고(커널 페이지 4KB라 동작에는 영향 없음). 사용자가 "다시 표시 안 함". 근본 해결은 자체 .so를 `-Wl,-z,max-page-size=16384`로 링크하고 서드파티 .so를 16KB 정렬판으로 교체(미실행).

### 19. 배터리 하한 — ⚠️
- 원칙 30–90%. Mix 4 E3 마지막 1회가 30→25%에서 진행. 문서에 표시.

## [T] 도구·환경

### 20. Windows에서 Mix 4가 adb에 안 보임 — ✅
- 장치 관리자에 "Pixel Tablet"(VID 18D1, libusbK 드라이버)로 잡혀 기본 adb(WinUSB)가 못 봄 → `ADB_LIBUSB=1`(래퍼 `adbw`). 이후 무선 adb(`adb tcpip 5555`)로 전환해 케이블 없이 측정. Wi-Fi를 껐다 켜도 같은 IP로 연결 유지됨을 확인.

### 21. Git Bash 경로 변환·heredoc·CRLF — ✅
- `MSYS_NO_PATHCONV=1`이면 `adb push /c/...`가 실패 → 로컬 경로는 `C:/...`로.
- bash heredoc 안 파이썬 문자열의 `\\n`이 `\n`으로 바뀌어 C 소스에 실제 줄바꿈이 들어감 → 패치 스크립트는 파일로 작성.
- Windows 파이썬이 CRLF로 저장 → 셸 스크립트 오동작 위험 → LF로 정규화.
- 실행 중인 bash 스크립트를 제자리 수정하면 깨질 수 있어 새 파일로 쓰고 `mv`(새 inode).

## 무효 처리한 런 (결과에 쓰지 않음, 원본은 zip의 `invalid_runs/`)
- Mix 4 E1_1027_1933, E2_1027_1955: AutoPowerKill로 샘플 중단(10번).
- Mix 4 E2_1027_2148: shell 샘플러 wake lock 없음(11번).
- C55 E4_1027_1921: 삼성 계정 팝업(18번), E4 첫 재시도 r1 이후: 캡처 OOM으로 샘플러 종료(5·6번).
