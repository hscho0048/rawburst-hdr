# Issue log — power measurement and Mi Mix 4 port (2026-10-27 – 28)

[한국어](power_issues.md) · Main document: [English](power.en.md) / [한국어](power.md)

Problems hit while taking the measurements in `docs/power.en.md`, written as **symptom → cause → evidence → fix → status**.
Categories: **[P] porting** (making the app and model work on the Mix 4), **[M] measurement infrastructure** (making unattended power measurement give correct numbers), **[T] tools and environment**.
Status: ✅ fixed / ⚠️ worked around (root cause remains) / ❌ unresolved.

## Summary

| # | Cat. | Issue | Status |
|---|---|---|---|
| 1 | P | Mix 4 NPU (HTP v68) cannot take the fp16 model and silently runs on the CPU | ✅ INT8 model puts 136/136 nodes on the NPU |
| 2 | P | The INT8 model fails to load on the device's LiteRT 2.16.1 | ✅ loads with the LiteRT 1.4.2 runtime (the app is still on 2.16.1) |
| 3 | P | Mask accuracy drops sharply on some scenes after INT8 quantisation | ⚠️ improved with calibration data, some scenes remain |
| 4 | P | Mix 4 27 MP RAW → app OOM | ✅ centre-crop to 4080×3072 |
| 5 | P | OOM on back-to-back capture without processing — also on the C55 | ✅ reuse slots |
| 6 | P | An app crash also kills the sampler in the same process | ✅ sampler moved to a separate `:power` process |
| 7 | M | The shell cannot read battery-current sysfs (both phones) | ⚠️ replaced with the BatteryManager path from the app/shell |
| 8 | M | `current now` from `dumpsys battery` is a cached value | ✅ not used |
| 9 | M | MIUI background restriction refuses to start the foreground service | ✅ allowed via appops |
| 10 | M | MIUI AutoPowerKill force-stops the measuring app after 7–16 min | ✅ shell-uid sampler |
| 11 | M | The shell sampler has no wake lock, so the phone suspends in screen-off windows | ✅ IPowerManager wake lock |
| 12 | M | Suspend during pre-cooling because the sampler had not started yet | ✅ start the sampler first |
| 13 | M | MIUI swipe lock turns the screen off after 10 s | ✅ `wm dismiss-keyguard` |
| 14 | M | `am force-stop` also kills the sampler service | ✅ reordered + flush every 5 s |
| 15 | M | Cool-down target never reached, so every window waited for the cap (10–15 min) | ⚠️ cap cut to 2 min, documented |
| 16 | M | Background activity contaminates the idle baseline (2 Mix 4 E2 windows) | ✅ automatic exclusion rule |
| 17 | M | App sampler voltage is the broadcast value, overestimating full load by 2.3% | ⚠️ quantified and documented |
| 18 | M | System pop-ups cover the app mid-experiment (Samsung account sign-in prompt, Android 16 16 KB warning) | ✅ handled by the user, re-measured |
| 19 | M | Windows below the 30% battery floor | ⚠️ marked in the document |
| 20 | T | The Mix 4 does not show up in adb on Windows | ✅ `ADB_LIBUSB=1` |
| 21 | T | Git Bash path conversion, heredoc escaping, CRLF | ✅ avoided by changing how files are written |

---

## [P] Porting

### 1. Mix 4 NPU cannot take the fp16 model and silently runs on the CPU — ✅
- **Symptom**: with `--delegate npu`, delegate creation is "ok" and inference works, but takes 7.5 ms (C55 NPU: 2 ms). Energy is 33.6 mJ per inference, the same as the CPU.
- **Cause**: the Snapdragon 888+'s Hexagon 780 = **HTP v68 has no fp16 ops** (fp16 HTP starts at v69). All 246 ops of the float16 model fail QNN validation, and TFLite runs the rejected ops on the CPU, so everything looks normal from outside.
- **Evidence**: 246 logcat lines `QnnDsp <E> Failed to validate op ... with error 0xc26` (Cast 110, Conv2d 43, Relu 22, …). E1 energy: "NPU" 33.6 mJ = CPU 33.3 mJ.
- **Fix**: `tools/quantize_selfie_int8.py` builds a static INT8 (a8w8) model (folds the 110 fp16 DEQUANTIZE ops into fp32 constants and adds a SignatureDef → ai-edge-quantizer 0.10). `seg_litert.cc` converts integer input/output and uses `kHtpQuantized` when the file name contains `int8`.
  Result: **136/136 nodes, 1 partition delegated to HTP v68**. Measured (whole device, net): **3.88 mJ / 4.15 ms** (2 runs) — 1/8.7 of the fp16 CPU fallback (33.6 mJ) and half of the Mix 4 GPU (7.55 mJ).
- **Lesson**: a successfully created delegate ≠ running on the NPU. Check the delegated node count (`Replacing N out of M node(s)`) and the actual J per inference. `seg_litert.cc` now logs the SoC → HTP version.
- Tried and dropped: **a16w8** (16-bit activations) — v68 does not support int16-activation Conv2d, DepthwiseConv, HardSwish, AvgPool or TransposeConv, giving 82/158 nodes, 124 partitions, 127 ms.

### 2. The INT8 model fails to load on the device's LiteRT 2.16.1 — ✅ (cli), not in the app
- **Symptom**: on both CPU and NPU, `ERROR: Input tensor 323 lacks data` → segmenter creation fails. Works on the PC with ai-edge-litert 2.3.
- **Cause**: the device's TFLite 2.16.1 runtime cannot handle the quantised graph produced by ai-edge-quantizer 0.10 (the same file works on a newer runtime). The op version fields were also left at v1 (CONV_2D v1 for an int16×8 conv).
- **Evidence**: swapping in `libtensorflowlite_jni.so` from the **LiteRT 1.4.2 AAR** (Google Maven `com.google.ai.edge.litert:litert:1.4.2`) via `LD_LIBRARY_PATH` loads the same file without rebuilding (C API compatible). The QNN delegate also works as is.
- **Fix**: the measurement cli uses 1.4.2 via `LIBDIR=/data/local/tmp/bp/l142` (`scripts/power_cli.sh`).
- **Remaining**: the app is still on 2.16.1 (fp16 model + GPU / NPU v69). Moving the app to 1.4.2 would change the runtime under the existing C55 measurements, so it was not done this round. Note: the 1.4.2 C API does not apply XNNPACK to the CPU path by default, so CPU inference slows to hundreds of ms → attach the XNNPACK delegate explicitly when migrating the app.

### 3. Mask accuracy drops sharply on some scenes after INT8 quantisation — ⚠️
- **Symptom**: IoU vs float (binarised at 0.5) is 0.83–0.95 on portrait scenes, but on a desk scene with no person (`test0`) the whole frame was judged foreground (mean |Δα| 0.6), and on a small-foreground scene IoU was near 0.
- **Cause**: the decoder's channel-gating path (global average pool → 1×1 conv → sigmoid → multiply) is fragile under **per-tensor int8**. Some calibration scenes widen this path's activation range a lot (e.g. `average_pooling2d_9` actually spans −2.7..1.5 but got a range of 66.7 → 0.26 per step), leaving too little resolution for other scenes. A known per-tensor quantisation problem for MobileNetV3-style models (SE + hard-swish).
- **Evidence**: a layer-by-layer comparison script (dequantises every intermediate tensor of both models and compares with float) shows the error first growing on this path.
- **Action**: calibration data changed to **real device bursts only** (frames 0/4/7 per burst + horizontal flips). Mean |Δα| over 7 real scenes: 0.081 (exposure augmentation 0.138, including emulator scenes 0.169). The `test0` misjudgement is gone (|Δα| 0.0001).
  Remaining: of the 7 scenes, one (small foreground) has IoU 0.0003 and one (bright panel) 0.55. Portrait scene 0.86.
- **Next steps (not done)**: a calibration set closer to real usage (hundreds of images), int16 only on the gating path (within what v68 supports: Mul/Add/Logistic), or QAT. In `docs/power.en.md` the NPU INT8 numbers are only given together with "accuracy differs".

### 4. Mix 4 27 MP RAW → app OOM — ✅
- **Symptom**: `java.lang.OutOfMemoryError` right after capture (MIUI log `JE_OOM`).
- **Cause**: the Mix 4 main camera offers **only 6016×4512 (27 MP, 108 MP sensor binned)** as `RAW_SENSOR`. 8 slots of 434 MB + a 108 MB output exceed the Java heap (512 MB). On Android `ByteBuffer.allocateDirect` is allocated from the Java heap.
- **Fix**: if RAW is over 16 MP, **crop the centre 4080×3072** into the slots (even offsets → CFA phase kept, `crop` in `meta.txt`). Same pixel count as the C55, which also keeps per-shot energy comparable.
- **Side effect (measured)**: the sensor still reads out 27 MP, so capture energy is 6× the C55 (0.58 vs 0.10 J per burst). Cropping only fixes memory.

### 5. OOM on back-to-back capture without processing — ✅ (also on the C55)
- **Symptom**: in E4's "capture only" stage the app died after the 2nd burst.
- **Cause**: 8×25 MB slots newly allocated per shutter press. Normally GC ran during the 400 ms of processing, so it passed by luck.
- **Fix**: the continuous-capture path reuses the previous slots (`CaptureController.lastSlots`). The normal capture path is unchanged to keep the same conditions as earlier measurements.

### 6. An app crash also kills the sampler in the same process — ✅
- **Symptom**: in the first C55 E4 attempt there were no samples after 16 min → every later window invalid; all of E4 re-measured.
- **Cause**: `PowerService` lived in the app's main process (taken down by the OOM in #5).
- **Fix**: separated with `android:process=":power"` in the manifest.

## [M] Measurement infrastructure

### 7. The shell cannot read battery-current sysfs — ⚠️
- **Symptom**: every `/sys/class/power_supply/*/current_now` is Permission denied (both C55 and Mix 4; battery/usb/ac/wireless/otg).
- **Alternatives checked**: thermal zone `vbat` (voltage) is readable. `*-ibat-lvl*` zones are battery-current-limit thresholds and inconsistent (lvl0 156 / lvl1 468). `service call batteryproperties` could not be parsed.
- **Fix**: the app's `BatteryManager.BATTERY_PROPERTY_CURRENT_NOW` (C55 and Mix 4 E1 r1–r2) → the same binder (`IBatteryPropertiesRegistrar.getProperty`) called directly from a shell `app_process` (rest of the Mix 4). The cli sysfs sampler is kept and records why it failed in the result JSON.

### 8. `current now` from `dumpsys battery` is a cached value — ✅
- Same value (−409 551) even when read 5 s apart. Not used for sampling; recorded only as a reference in the brightness check.

### 9. MIUI background restriction refuses to start the foreground service — ✅
- **Symptom**: on the Mix 4, samples stopped after 15 s and a 60 s idle window stretched to 174 s (the phone suspended).
- **Cause**: app default `RUN_ANY_IN_BACKGROUND=ignore` → `Service.startForeground() not allowed due to bg restriction` → the service never became foreground and was frozen.
- **Fix**: `cmd appops set dev.burstpipe RUN_ANY_IN_BACKGROUND allow` + battery-optimisation exemption, set by the runner every time.

### 10. MIUI AutoPowerKill force-stops the measuring app — ✅
- **Symptom**: 7–16 min after the screen turns off, samples stop and every later window is invalid (first Mix 4 E1 and E2 attempts).
- **Cause**: `dumpsys activity exit-info` → `stop dev.burstpipe due to AutoPowerKill`. Foreground services (importance 125) are targets too. It recurred even after setting the app's battery option to "No restrictions" (possibly reset by an APK reinstall right after).
- **Fix**: run the sampler as a **shell-uid process**: `CLASSPATH=$(pm path dev.burstpipe) app_process /system/bin dev.burstpipe.ShellSampler`.
  Check: recording the same windows with both, it agrees with the app sampler within 0.1–0.3% in idle and GPU windows.

### 11. The shell sampler has no wake lock and the phone suspends — ✅
- **Symptom**: in the Mix 4 E2 retry the CSV was 54 KB after 10 min (1/10 of normal) and iterations stretched to 3–14 s.
- **Cause**: the app service held a PARTIAL wake lock, but the shell process did not. `/sys/power/wake_lock` is not writable by the shell.
- **Fix**: the shell uid has the `WAKE_LOCK` permission, so `IPowerManager.acquireWakeLock` is called by reflection (`burstpipe:shell_sampler` confirmed in `dumpsys power`).

### 12. Suspend during pre-cooling because the sampler had not started — ✅
- **Symptom**: in Mix 4 E3, pre-cooling (max 5 min) ran for over 12 min.
- **Cause**: the runner measured the reference temperature with the screen off before starting the sampler (= the wake lock).
- **Fix**: the shell sampler is not affected by force-stop, so it now starts at the very beginning of the runner.

### 13. MIUI swipe lock turns the screen off after 10 s — ✅
- **Symptom**: the Mix 4 screen turned off even though the app sets `FLAG_KEEP_SCREEN_ON`, and the brightness-check windows read the same as screen-off power.
- **Cause**: `PowerManagerService: Going to sleep due to timeout (screenOffTimeout=600000, activityTimeoutWM=10000)` — 10 s while the keyguard is showing.
- **Fix**: `wm dismiss-keyguard` after waking (same as a swipe; no settings changed).

### 14. `am force-stop` also kills the sampler service — ✅
- The runner force-stopped the app (to restart it) after starting the sampler → reordered (force-stop → sampler → app), and at the end the sampler is stopped first. The service appends to the file every 5 s.

### 15. Cool-down target never reached — ⚠️
- **Symptom**: the "start temperature + 1 °C" target was never met with the camera on (C55 E4) or during background activity (Mix 4 E2), so every window waited for the cap (10–15 min) → wasted time and battery.
- **Cause**: with the camera preview on, the hottest CPU sensor swings several °C within a second. The start temperature had been taken when the phone was very cool.
- **Action**: app E4 cap 2 min, cli cap `COOL_MAX`, pre-cooling max 5 min. Only E3 turns the screen off and cools for up to 10 min. Alternating order + subtracting the idle right before reduce the bias, but stages did not run at identical temperatures (recorded in the limitations).

### 16. Background activity contaminates the idle baseline — ✅
- **Symptom**: idle of 2.3–3.0 W in two Mix 4 E2 r3 windows (normally 0.30 W).
- **Cause (estimate)**: MIUI background work (dex optimisation, security scan) after an APK reinstall just before. CPU temperature was 43 °C at idle at the same time.
- **Fix**: analysis rule — if idle_W exceeds 2 × the median idle of the same analysis, the window is excluded from the summary (raw data kept). Zero exclusions in every other run.

### 17. The app sampler voltage is the broadcast value — ⚠️
- `EXTRA_VOLTAGE` only updates on broadcasts (minutes apart). Recording concurrently with the shell sampler (live vbat) on the Mix 4: under full CPU load the app reads +2.3% (misses the 8.07→7.87 V drop); idle and GPU within 0.1–0.3%. The C55 numbers still carry this error.

### 18. System pop-ups during experiments — ✅
- C55: a Samsung account sign-in confirmation covered the app → it is an authentication screen, so it was left to the user; the affected run was discarded and re-measured.
- C55 (Android 16): a "not 16 KB page aligned" compatibility warning for the debug app (the kernel page size is 4 KB, so it has no effect on running). The user chose "Don't show again". The real fix is to link our own .so with `-Wl,-z,max-page-size=16384` and replace third-party .so files with 16 KB-aligned builds (not done).

### 19. Battery floor — ⚠️
- Rule: 30–90%. The last Mix 4 E3 run went from 30% to 25%. Marked in the document.

## [T] Tools and environment

### 20. The Mix 4 does not show up in adb on Windows — ✅
- Device Manager lists it as "Pixel Tablet" (VID 18D1, libusbK driver), which the default adb (WinUSB) cannot see → `ADB_LIBUSB=1` (wrapper `adbw`). Then switched to wireless adb (`adb tcpip 5555`) to measure without a cable. Confirmed the connection survives turning Wi-Fi off and on (same IP).

### 21. Git Bash path conversion, heredoc, CRLF — ✅
- With `MSYS_NO_PATHCONV=1`, `adb push /c/...` fails → use `C:/...` for local paths.
- `\\n` inside a Python string in a bash heredoc became `\n`, putting real newlines into C source → patch scripts are written as files.
- Python on Windows saved files with CRLF → risk of broken shell scripts → normalised to LF.
- Editing a running bash script in place can break it → write a new file and `mv` it (new inode).

## Runs marked invalid (not used in results; raw data kept outside the repo)
- Mix 4 E1_1027_1933, E2_1027_1955: samples stopped by AutoPowerKill (#10).
- Mix 4 E2_1027_2148: shell sampler without a wake lock (#11).
- C55 E4_1027_1921: Samsung account pop-up (#18); first E4 retry after r1: sampler died with the capture OOM (#5, #6).
