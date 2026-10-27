# Power and energy measurement — Galaxy C55 / Xiaomi Mi Mix 4

[한국어](power.md) · Issue log: [English](power_issues.en.md) / [한국어](power_issues.md)

> **Every power number here is whole-device (battery) power.** Retail Samsung and Xiaomi phones have no per-rail power meter (like Pixel's ODPM for SoC/DDR/camera),
> so we measure the total power leaving the battery. It is not SoC-only power: the display, modem (airplane mode), camera module and DRAM are all included.
> The main metric is therefore **net energy (net J)** — the same-condition idle subtracted — and **net J per iteration**.
> Only measured values are reported. Estimates are marked "estimate", and experiments that did not work are marked "did not work + reason".

Source labels: **[C55]** measured on Galaxy C55, **[Mix4]** measured on Mi Mix 4. Analysis JSONs (gross/idle/net J, iterations, ms, temperature and clocks per window) are in `results/power/<device>/`.
Raw data (50 ms battery-sample CSVs, window records, 1 s clock/temperature logs) is too large for the repository and kept separately (`results/power/<device>/raw/`, gitignored). Running the scripts recreates it in the same place.

## 1. Devices

| | Galaxy C55 (SM-C5560) | Xiaomi Mi Mix 4 (2106118C) |
|---|---|---|
| SoC | Snapdragon 7 Gen 1 (SM7450, taro) | Snapdragon 888+ (SM8350, lahaina; X1 2.995 GHz) |
| CPU | 0–3 A510 1.80 GHz / 4–6 A710 2.36 GHz / 7 A710 2.40 GHz | 0–3 A55 1.80 GHz / 4–6 A78 2.42 GHz / 7 X1 2.995 GHz |
| NPU (Hexagon HTP) | v69 — supports fp16 | **v68 (Hexagon 780) — no fp16, integer only** |
| OS | Android 16 (One UI) | Android 14, HyperOS OS1.0.8.0 (kernel 5.4) |
| Battery | 1 cell, ~4.3 V | **2 cells in series, ~8.6 V** (120 W charging design) |
| Display | 1080×2400 | 1080×2400, 120 Hz |
| Main camera RAW | 4080×3072 (50 MP, 2×2 binned), GBRG, ISO 100–6400 | **6016×4512 (108 MP sensor binned; the only RAW size offered)**, GRBG, ISO 50–1600 |
| BatteryManager CURRENT_NOW | µA, **discharge negative** | µA (quantised to 1 mA), **discharge positive** |

(Mix 4 camera static info: `docs/camera_dump_mix4.txt`; C55: `docs/camera_dump.txt`)

## 2. Method

### 2.1 Sampler — why we measure from an app
- cli (`core/src/power.cc` `BatterySampler`): one thread reads `/sys/class/power_supply/*/current_now` and `voltage_now` every 50 ms and integrates P = V × I with the trapezoid rule.
  **On both phones the shell gets `Permission denied`** (battery, usb, ac, wireless, otg — recorded in the validation windows' `sysfs_power.why`).
  The thermal zone `vbat` (voltage) is readable, but the current zones (`*-ibat-lvl*`) are battery-current-limit thresholds and not consistent (Mix 4: lvl0 156 / lvl1 468), so we did not use them.
  `current now` from `dumpsys battery` (Samsung only) returns the same cached value even 5 s apart, so it cannot be used for sampling.
- So **the app's `PowerService` (foreground service)** reads `BatteryManager.BATTERY_PROPERTY_CURRENT_NOW` every 50 ms, and takes voltage from `EXTRA_VOLTAGE` of `ACTION_BATTERY_CHANGED`.
  CSV: `t_ns (CLOCK_BOOTTIME), current_raw, charge_uah, voltage_mv, temp_dc, plugged, status`, appended to the file every 5 s.
- In cli experiments the cli records **only the window boundaries** (`--power-json`: `CLOCK_BOOTTIME` ns, the same clock as the app's `elapsedRealtimeNanos`, plus iterations, ms, temperature and clocks).
  On the PC, `tools/power_analyze.py` integrates samples over each window. App experiments (E3, E4) write window JSONs in the same format.
- Per window: `gross_J` = ∫V·I dt, `idle_W` = mean of the 30 s same-condition idle window right before, `net_J = gross_J − idle_W × window length`, `net J/iter = net_J / iterations`.

### 2.2 Validation (unit, sign, update period) — `results/power/<device>/calibration.json`

| | C55 | Mix 4 |
|---|---|---|
| Unit | µA (screen-off idle median −40 283 → 0.22 W at 4.3 V) | µA (34 000 → 0.30 W at 8.6 V; quantised to 1 mA) |
| Discharge sign | **negative** | **positive** |
| Real fuel-gauge update period (interval between value changes) | median 250 ms (p10–p90 200–1000 ms) | median 250 ms (p10–p90 246–498 ms) |
| Sample interval | 50.0 ms | 50.0 ms (no gaps; see 2.4) |
| Direction check with screen brightness | screen off 0.22 W < brightness min 0.68 W < max 1.05 W | app window brightness 0.0/1.0 alternated 6× for 15 s each: max is **+0.060–0.075 W** above the neighbouring mins (5/5) |
| Coulomb counter (CHARGE_COUNTER) cross-check | too coarse for 30 s windows (74 J vs 37–66 J for the same window) | same |

- Update period 250 ms ≪ 30 s windows → every experiment measures **the same work repeated for at least 30 s** (a single 2 ms inference cannot be measured directly).
- Voltage only updates on broadcasts (minutes apart), so it does not follow the voltage drop under load → energy in loaded windows is **overestimated by about 1–2% (estimate)** (tens of mV out of 4.3 V), partly cancelled by idle subtraction.

### 2.3 Rules (both phones)
- **USB cable unplugged.** With the cable in, charging current is mixed in (`dumpsys battery unplug` only changes what is displayed). Control goes over wireless adb (`adb tcpip` → `adb connect`),
  and the experiment sequence runs on the phone in a shell runner started with nohup, so no adb/Wi-Fi traffic lands inside a window. The PC only checks a status file once a minute. Every window is checked for `plugged=0`.
- Battery 30–90% (level recorded per run), airplane mode with Wi-Fi only for adb, Bluetooth off.
- cli experiments (E1, E2) with the screen off; app experiments (E3, E4) with the screen on, window brightness fixed at 0.4, camera preview running.
- A 30 s same-condition idle baseline before every window. Cool-down: until the hottest CPU zone is within +1 °C of the start temperature (capped at 10–15 min). Pre-cooling before the start (until the temperature stops falling by more than 0.5 °C per minute).
- 3 runs, order alternated (A B C A B C …; in the app, odd runs forward and even runs reversed); tables show the median (min–max).
- Clocks: every 200 ms during a window, cpu0/4/7 `scaling_cur_freq` (mean) and `scaling_max_freq` (minimum = throttle cap).

### 2.4 Problems running unattended (both phones)
- **MIUI background restriction**: the app default is `RUN_ANY_IN_BACKGROUND=ignore` → `Service.startForeground() not allowed due to bg restriction` →
  the service never became foreground and froze after 15 s, leaving a 363 s gap in the samples; the phone also suspended, stretching a 60 s idle window to 174 s.
  Fixed with `cmd appops set dev.burstpipe RUN_ANY_IN_BACKGROUND allow` + battery-optimisation exemption (`dumpsys deviceidle whitelist +`). No gaps afterwards.
- **MIUI swipe lock**: after `input keyevent WAKEUP`, if the keyguard is showing, `activityTimeoutWM=10000` turns the screen off in 10 s (regardless of the 10 min timeout setting).
  After `wm dismiss-keyguard` (same as a swipe; no settings changed) the app's `FLAG_KEEP_SCREEN_ON` holds.
- `am force-stop` also kills the sampler service of the same package → the runner starts the sampler after the force-stop and stops the sampler first at the end. The service writes to file every 5 s.
- On the Windows PC the Mix 4 binds to a libusbK driver ("Pixel Tablet") and does not show up in `adb devices` → `ADB_LIBUSB=1`.

## 3. Results — Galaxy C55 [C55]

Conditions: cable unplugged, airplane mode + Wi-Fi, battery 88→40% (E1 88–87, E2 87–85, E4 69–54, E3 52–40), indoors around 23 °C.
Sampler: app `PowerService` (BatteryManager, 50 ms). Input: `bursts/device/test0` (C55 RAW 4080×3072 × 8).

### E1. Segmentation backend — per inference, whole device (battery)
cli `--seg-only` (the 256² input is prepared once outside the window; only inference is repeated for ≥30 s), screen off. Model: `selfie_segmenter_std.tflite` (float16, 246 nodes).

| Backend | **net mJ/inference** | net W | ms/inference | mean W (gross) | idle W | CPU °C start→end |
|---|---|---|---|---|---|---|
| **NPU** (QNN HTP v69, fp16, 246/246) | **4.43** (4.31–4.52) | 2.24 | **1.99** | 2.47 | 0.23 | 36→46 |
| GPU (OpenCL, 246/246) | 8.31 (8.07–8.83) | **0.52** | 16.0 | 0.75 | 0.20 | 36→34 |
| CPU (XNNPACK, 4 threads) | 40.9 (40.2–41.6) | 4.42 | 9.22 | 4.68 | 0.22 | 39→66 |

- The NPU is **8× faster than the GPU with 1.9× less energy**. Its mean power (2.24 W) is 4× the GPU's (0.52 W), but each inference takes 1/8 of the time, so energy per inference is half.
  → "lowest power (W)" and "lowest energy (J)" are different things. When the unit of work is fixed, compare J per unit.
- The CPU is faster than the GPU (9 ms) but costs 41 mJ per inference (9.2× the NPU) and reaches 66 °C within 30 s. Sharing the big cores with the merge also slows the merge (earlier measurements, ch. 4).
- (Note) The earlier 370 ms CPU figure was for the MediaPipe custom-op model with our own reference kernel. With the model converted to standard `TRANSPOSE_CONV`, XNNPACK takes the whole graph: 9.2 ms.

### E2. Merge (align + merge + finish) core placement — per burst, whole device (battery)
cli, screen off, no segmentation or bokeh, 4080×3072 × 8. C55 cores: 0–3 A510 / 4–6 A710 / 7 A710 (highest clock).

| Placement | **net J/burst** | ms/burst | net W | cpu4·cpu7 mean / cap MHz | CPU °C |
|---|---|---|---|---|---|
| scalar, 1 thread (cpu7) | 2.61 (2.56–2.74) | 1519 | 1.71 | –· 2400 / 2400 | 43→48 |
| NEON, 4 little (A510, cpu0–3) | 2.19 (2.06–2.23) | 1723 | 1.25 | (cpu0 1804) | 37→42 |
| NEON, cpu7 alone (A710) | 1.81 (1.72–1.84) | 932 | 1.94 | –· 2400 / 2400 | 42→50 |
| **NEON, 4 big (A710, cpu4–7)** | **1.67** (1.66–1.68) | 318 | 5.24 | 2157·2126 / **2054·2169** | 53→**71** |
| NEON, 8 threads unpinned | 1.72 (1.68–1.74) | **295** | 5.84 | 2124·2044 / **1651·1766** | 55→67 |

- **Race-to-idle wins.** The 4 big cores are 5.4× faster than the 4 little cores and use 24% less energy (1.67 vs 2.19 J). The little cores draw less power (1.25 W), but DRAM, L3 and shared SoC power keep flowing
  for 1.7 s, so J per burst goes up. This work is memory-bandwidth heavy (merge IPC 2.6, 42 M LLC misses, earlier measurements), which makes "slow on efficient cores" a losing choice.
- NEON vs scalar (same cpu7, 1 thread): 932 vs 1519 ms, 1.81 vs 2.61 J → **SIMD cuts time by 39% and energy by 31%.**
- 8 unpinned threads are 23 ms faster than the 4 big cores but wake the little cores too and use 0.05 J more. Within 30 s the clock cap drops to 1651/1766 MHz (4 big: 2054/2169) —
  under sustained load the thermal budget is shared with the little cores and the big cores lose more.
- Caveat: the big-core windows hit the first throttle step (2361→2054 MHz) within 30 s. The table is for "30 s of continuous processing"; a single shutter press from cold will be faster and cheaper (estimate).

### E4. Per-stage energy of one app shot — whole device (battery), relative to camera preview running
App (Kotlin Camera2 + JNI), screen on, window brightness 0.4, NPU segmentation. Each stage is repeated for ≥30 s, and the "preview only" 30 s window right before is subtracted.

| Stage | **net J/iter** | ms/iter | net W | Note |
|---|---|---|---|---|
| Capture (8-frame RAW burst, no processing) | 0.10 (0.05–0.11) | 518 | 0.14 | the preview already keeps the sensor running, so the extra cost is small |
| Merge + finish (plain) | 1.21 (1.19–1.36) | 475 | 2.55 | |
| Merge + segmentation + bokeh (portrait) | 1.53 (1.45–1.66) | 578 | 2.64 | |
| Segmentation only (pre-processing + NPU inference) | 0.008 | 7.3 | 1.14 | |
| JPEG (Bitmap + JPEG 95 file write) | 0.18 (0.17–0.21) | 199 | 0.97 | |
| Bokeh + mask refinement | **0.31 (estimate)** | — | — | = portrait − plain − segmentation (difference) |
| **Keeping the preview running (idle)** | **2.76 W** | — | — | display + sensor streaming + ISP; **≈3.3 J** over a 1.2 s shutter→JPEG |

- One portrait shot = capture 0.10 + processing 1.53 + JPEG 0.18 ≈ **1.8 J (net, excluding preview)**. Over the same time the preview uses ≈3.3 J →
  **in the whole-device energy of one shot, keeping the preview running costs more than the processing.** Lowering the preview frame rate or resolution while processing
  may save more per shot than optimising the processing (e.g. a 10% faster merge saves 0.12 J) — estimate, not measured.
- NPU segmentation costs 8 mJ per shot, 3% of the portrait − plain difference (0.31 J). Most of the extra cost of portrait mode is bokeh (mask refinement, blur, composite on the CPU).

### E3. 150 consecutive shots — whole device (battery), shutter → processing → JPEG, segmentation and bokeh off
Before every run the screen is turned off and the phone cools to the screen-off reference (34 °C) + 1 °C (max 10 min). Policy off = fixed N=8; thermal = latency-feedback governor (target 450 ms). **2 runs** (reduced from 3 for battery and time).

| Policy | total net J for 150 | **net J/shot** | shutter→JPEG ms/shot | window s | mean W | preview idle W |
|---|---|---|---|---|---|---|
| off (fixed N=8) | 320 (303–337) | 2.13 (2.02–2.25) | 1282 | 200 | 4.37 | 2.77 |
| thermal (governor) | 316 (308–324) | 2.11 (2.05–2.16) | 1277 | 199 | 4.45 | 2.87 |

In blocks of 30 shots (run r1 of 2; `results/power/c55/E3_1027_2140_detail.json`):

| Shots | off processing ms | off shutter→JPEG | thermal processing ms | thermal shutter→JPEG | thermal mean N |
|---|---|---|---|---|---|
| 1–30 | 430 | 1203 | 411 | 1187 | 8 |
| 61–90 | 446 | 1267 | 435 | 1254 | 8 |
| 91–120 | 491 | 1323 | 478 | 1328 | 8 |
| 121–150 | **524** | **1368** | **483** | **1330** | **7.5** |

- Throttling comes in two steps: the big-core cap drops 2361→2054 MHz at **shot 6–8 (7–9 s)**, and the deep step to 1555 MHz at **shot 68–79 (86–99 s)**
  (matching the earlier "from shot 81, +28%" measurement). CPU peak 66–70 °C.
- The governor only lowered N from 8 to 7 (mean 7.3–7.5) after shot 121, when the processing-time EMA exceeded 450 ms × 1.1, cutting processing in the last 30 shots by **−41 ms (−8%)**.
  The per-shot energy difference over all 150 shots is **−1% (2.13→2.11 J), within run-to-run spread (±0.1 J)** — no energy saving can be claimed at this length.
  The governor's benefit is keeping a latency ceiling (QoS); its energy effect needs to be measured again over longer throttled use (hundreds of shots).
- Of the 5.6–6.1 J gross per shot (preview included), the preview is ≈3.5 J (2.77 W × 1.28 s). Same conclusion as E4.

## 4. Results — Xiaomi Mi Mix 4 [Mix4]

Conditions: cable unplugged, airplane mode + Wi-Fi, battery from 90% down to the low 70s, indoors. E1 and E2 use **the same RAW burst dumped on the C55** (`test0`, 4080×3072×8) pushed to the phone — the same input on both SoCs.
Sampler: app `PowerService` for E1 r1–r2; after that (E1 r3 GPU/CPU, E2, E3, E4) the **shell sampler** (`ShellSampler`, see section 6). Recording the same windows with both: 0.1–0.3% difference in idle and GPU windows;
in full-CPU-load windows the shell sampler reads 2.3% lower (it follows the live voltage drop 8.07→7.87 V, while the app holds the broadcast voltage) — confirming the "1–2% overestimate (estimate)" from 2.2 by measurement.
**MIUI keeps the big-core caps at cpu4–6 2112 MHz and cpu7 (X1) 2150 MHz throughout** (spec 2419 / 2995 MHz, `scaling_max_freq`). So the X1 results are at 2.15 GHz.

### E1. Segmentation backend — per inference, whole device (battery)

| Backend | **net mJ/inference** | net W | ms/inference | mean W | idle W | CPU °C |
|---|---|---|---|---|---|---|
| NPU requested → **actually CPU fallback** (HTP v68 has no fp16; all 246 ops fail validation) | 33.6 (33.5–34.5) | 4.46 | 7.57 | 4.76 | 0.30 | 33→57 |
| **GPU** (OpenCL, Adreno 660, 246/246) | **7.55** (7.43–7.62) | 0.46 | 16.5 | 0.75 | 0.29 | 33→30 |
| CPU (XNNPACK, 4 threads) | 33.3 (32.8–33.5) | 4.36 | 7.62 | 4.66 | 0.30 | 33→58 |
| **NPU INT8** (HTP v68, a8w8, **136/136 nodes, 1 partition**, LiteRT 1.4.2) | **3.88** (3.85–3.91) | 0.93 | 4.15 | 1.33 | 0.40 | 37→35 |

- INT8 row: measured on 2026-10-28 (battery 90%, same procedure, 2 runs — r1 had a 3.17 W idle from background activity and was excluded automatically). **Half the GPU's energy**, slightly below the C55 fp16 NPU (4.43 mJ).
  ms/inference is higher than in a quick single test (1.3 ms) because, with the screen off, the CPU clock is ~850 MHz and the input quantisation and output dequantisation run on the CPU.
  **But INT8 masks differ from float**: on 7 real scenes, portrait IoU 0.86, one small-foreground scene 0.0003, one bright-panel scene 0.55 ([issue 3](power_issues.en.md)). It is not used in the product path until accuracy is fixed.
- The "NPU" (fp16) J and ms equal the CPU → the fallback is confirmed by energy too. Creating the delegate succeeds ("ok"), so **without reading the logs you would think it runs on the NPU** (section 6, #1).
- With the float model, the most efficient path on the Mix 4 is the GPU (7.55 mJ) — similar to the C55 GPU (8.31 mJ) and 1.7× the C55 NPU (4.43 mJ). With the INT8 model, the NPU (3.88 mJ) is lower still.

### E2. Merge core placement — per burst, whole device (battery)
Mix 4 cores: 0–3 A55 1.80 GHz / 4–6 A78 (capped 2.11 GHz) / 7 X1 (capped 2.15 GHz).

| Placement | **net J/burst** | ms/burst | net W | CPU °C | runs |
|---|---|---|---|---|---|
| scalar, 1 thread (X1) | 2.29 (2.26–2.30) | 1426 | 1.61 | 36→41 | 3 |
| NEON, 4 little (A55) | 1.68 (1.67–1.70) | 2104 | 0.80 | 31→34 | 3 |
| NEON, 3 mid (A78) | 1.61 (1.61–1.61) | 314 | 5.12 | 43→63 | 2* |
| **NEON, X1 alone** | **1.52** (1.52–1.52) | 795 | 1.91 | 37→43 | 2* |
| NEON, mid + X1 (4) | 1.62 (1.62–1.64) | 258 | 6.28 | 42→71 | 3 |
| NEON, 8 unpinned | 1.61 (1.61–1.64) | **246** | 6.55 | 46→71 | 3 |

\* Two r3 windows had an idle baseline of 2.3–3.0 W (normally 0.30 W), contaminated by background activity (likely MIUI background work after an APK reinstall) → excluded by the analysis rule (idle > 2 × the analysis' median idle).

- **The conclusion differs from the C55.** On the Mix 4 the NEON placements are within 1.52–1.68 J (±5%), and the lowest is **X1 alone** (one fast core, 0.8 s).
  The 4 little cores (A55) are 6.7× slower yet use only 10% more energy. On the C55 the little cores (A510) used 31% more than the big ones.
  → The answer to "finish fast and idle vs. run long on efficient cores" depends on the SoC (core microarchitecture, process, shared L3/DRAM power), and **the same code has to be measured per device.**
- 3 mid, mid + X1, and 8 unpinned take a third of the time but run at 6–6.5 W and reach 70 °C — under 30 s of sustained load their energy is within 6% of X1 alone, but the thermal headroom is very different.
- scalar → NEON (X1, 1 thread): 1426 → 795 ms (−44%), 2.29 → 1.52 J (−34%).

### E4. Per-stage energy of one app shot — whole device (battery), relative to camera preview running
Mix 4's own captures (27 MP RAW → centre 4080×3072), segmentation on the **GPU** (the app still uses LiteRT 2.16, so the INT8 NPU path is not in the app), window brightness 0.4, battery 66→44%.

| Stage | **net J/iter** | ms/iter | net W | C55 (for reference) |
|---|---|---|---|---|
| Capture (8 RAW frames) | **0.58** (0.58–0.59) | 499 | 1.16 | 0.10 J / 518 ms |
| Merge + finish (plain) | 1.23 (1.21–1.24) | 364 | 3.40 | 1.21 J / 475 ms |
| Merge + segmentation + bokeh (portrait) | 1.51 (1.49–1.53) | 460 | 3.30 | 1.53 J / 578 ms |
| Segmentation only (pre-processing + GPU) | 0.009 | 9.6 | 0.91 | 0.008 J (NPU) / 7.3 ms |
| JPEG | 0.18 (0.17–0.19) | 155 | 1.18 | 0.18 J / 199 ms |
| Bokeh + mask refinement | **0.27 (estimate, difference)** | — | — | 0.31 J |
| **Keeping the preview running (idle)** | **3.14 W** | — | — | 2.76 W |

- Capture costs **6×** the C55 (0.58 vs 0.10 J). The Mix 4 only offers 27 MP RAW, so the sensor reads out and transfers 27 MP every frame, and the app crops it row by row into Java buffers (porting #2).
  Unless the sensor can deliver a 12 MP mode directly, cropping only fixes memory, not energy.
- Processing (plain/portrait) costs almost the same on both phones (1.21–1.23 / 1.51–1.53 J); the Mix 4 is 23% faster but at higher net power, 3.3–3.4 W (C55 2.55–2.64 W).
- With a 3.14 W preview, on the Mix 4 too the preview energy for one shot (shutter→JPEG about 1.0–1.5 s, ≈3–4.5 J) exceeds the processing (≈2.3 J: capture 0.58 + portrait 1.51 + JPEG 0.18).

### E3. 150 consecutive shots — whole device (battery), shutter → processing → JPEG, segmentation and bokeh off
Same procedure as the C55 (cool to the screen-off reference 34 °C + 1 °C before each run, 2 runs). Battery 42→**25%** — the last run (r2 off) finished below the 30% floor.

| Policy | total net J for 150 | **net J/shot** | shutter→JPEG ms/shot | processing ms (shots 1–30 → 121–150) | mean N | preview idle W |
|---|---|---|---|---|---|---|
| off (fixed N=8) | 350 (344–356) | 2.33 (2.30–2.37) | 1282 | 387 → 387–396 | 8 | 3.19 |
| thermal (governor) | 351 (344–357) | 2.34 (2.29–2.38) | 1277 | 386 → 389–391 | 8 | 3.15 |

- **Processing time stays flat for all 150 shots (387 ms).** While the camera app is open, MIUI caps the big cores and the X1 at **1555 MHz from the start**
  (monitor log: cap4 and cap7 = 1555 MHz already 29 s before the start; 2112/2150 during cli experiments). With no lower step left to fall to, throttling never changes,
  and the governor never exceeded its target (450 ms) so it never changed N → both policies ended up as the same experiment (difference +0.4%, within spread).
- The C55 goes "fast at first → deep throttle at 86–99 s → processing +22%", while the Mix 4 is "slow cap from the start, constant". When **the OS already applies a thermal policy**,
  an app-level governor has nothing to do. The user-visible shutter→JPEG (1.28 s) is the same on both phones.

## 5. Comparing the two phones — whole device (battery)

> **The SoC is not the only difference**: OS (One UI/Android 16 vs HyperOS/Android 14), battery (1 cell 4.3 V vs 2 cells 8.6 V), display panel, camera sensor (50 MP vs 108 MP; both processed at 4080×3072),
> and kernel governor / thermal policy (Mix 4 big-core caps fixed at 2.11/2.15 GHz) all differ. Do not read the differences below as "Snapdragon 7 Gen 1 vs 888+" alone.
> E1 and E2 use the same input (C55 RAW `test0`), screen off and idle subtraction, so they are fairly comparable; E3 and E4 use each phone's own captures.

| Item (net, median) | Galaxy C55 (SM7450) | Mi Mix 4 (SM8350) |
|---|---|---|
| Screen-off idle | 0.22 W | 0.30 W |
| E1 segmentation NPU | **4.43 mJ / 1.99 ms** (fp16) | fp16 impossible (CPU fallback 33.6 mJ) → **INT8 3.88 mJ / 4.15 ms** (accuracy differs) |
| E1 segmentation GPU | 8.31 mJ / 16.0 ms | **7.55 mJ** / 16.5 ms |
| E1 segmentation CPU, 4 threads | 40.9 mJ / 9.22 ms | 33.3 mJ / 7.62 ms |
| E1 most efficient path | NPU fp16 4.43 mJ | float model: GPU 7.55 mJ / INT8: NPU 3.88 mJ |
| E2 scalar, 1 thread (highest-clock core) | 2.61 J / 1519 ms | 2.29 J / 1426 ms |
| E2 NEON, 4 little | 2.19 J / 1723 ms | 1.68 J / 2104 ms |
| E2 NEON, one highest-clock core | 1.81 J / 932 ms (A710 2.40 GHz) | **1.52 J** / 795 ms (X1 2.15 GHz) |
| E2 NEON, 4 big | **1.67 J** / 318 ms | 1.62 J / 258 ms (A78×3 + X1) |
| E2 NEON, 8 unpinned | 1.72 J / 295 ms | 1.61 J / 246 ms |
| E2 lowest-energy placement | 4 big (race-to-idle) | X1 alone (placements within ±5%) |
| E2 little vs lowest | +31% | +10% |
| E4 keeping the preview (display + sensor + ISP) | 2.76 W | 3.14 W |
| E4 capture, 8 RAW frames | 0.10 J | **0.58 J** (27 MP readout + crop) |
| E4 merge + finish / portrait processing | 1.21 / 1.53 J (475 / 578 ms) | 1.23 / 1.51 J (364 / 460 ms) |
| E4 one segmentation (app, incl. pre-processing) | 8 mJ (NPU) | 9 mJ (GPU) |
| E4 JPEG | 0.18 J | 0.18 J |
| E3 150 shots, net J/shot (off / governor) | 2.13 / 2.11 J | 2.33 / 2.34 J |
| E3 shutter→JPEG (median of 2 runs; identical by coincidence — per-run C55 1262·1302 / Mix 4 1249·1315) | 1282 / 1277 ms | 1282 / 1277 ms |
| E3 throttling | step 1 at 7–9 s (2361→2054), step 2 at 86–99 s (→1555 MHz), processing 430→524 ms | fixed at 1555 MHz from the start while the camera is open, processing constant at 387 ms |

## 6. Porting to the Mi Mix 4 — problems and fixes

| # | Problem | Cause | Fix / status |
|---|---|---|---|
| 1 | **The NPU silently runs on the CPU** (delegate creation "ok", inference 7.5 ms) | Hexagon 780 = HTP **v68 has no fp16** (fp16 HTP starts at v69). All 246 ops of the float16 model fail `QnnBackend_validateOpConfig failed 0xc26` → TFLite runs everything on the CPU | **INT8 a8w8 model + LiteRT 1.4.2: 136/136 nodes, 1 partition on the NPU, 3.88 mJ / 4.1 ms. Accuracy issue remains — [issues 1–3](power_issues.en.md).** `fetch_qnn.sh` packages both v68 and v69 Stub/Skel; `seg_litert.cc` logs the HTP version from `ro.soc.model` (SM8350→v68) and supports int8/int16 I/O and `kHtpQuantized`. The device's LiteRT 2.16 cannot load the quantised model; the app still uses GPU segmentation on the Mix 4 |
| 2 | **OutOfMemoryError** right after capture | the main camera's RAW_SENSOR is **6016×4512 (27 MP) only** (108 MP sensor binned). 8 slots of 434 MB + output buffers exceed the 512 MB Java heap (on Android `allocateDirect` comes from the Java heap) | if RAW is over 16 MP, **crop the centre 4080×3072** into the slots (even offsets → CFA phase kept, `crop` in `meta.txt`). Same pixel count as the C55, so per-shot energy compares under the same conditions. Shutter→JPEG 1.50 s (capture 806 ms: 27 MP readout) |
| 3 | OOM when only capturing back-to-back (no processing) | 8×25 MB slots newly allocated per shutter press. Normally the 400 ms of processing gave GC time to catch up, so it passed by luck (also reproduced on the C55) | the continuous-capture path reuses the previous slots (`CaptureController.lastSlots`) |
| 4 | The measuring app dies after 7–16 min | **MIUI PowerKeeper `AutoPowerKill`**: force-stops even a foreground service after the screen turns off (`dumpsys activity exit-info`: "stop dev.burstpipe due to AutoPowerKill"). Recurred even with the app's battery setting on "No restrictions" | run the sampler **as a shell-uid process** (`app_process` + `ShellSampler`): `IBatteryPropertiesRegistrar.getProperty(CURRENT_NOW)` (the same value as BatteryManager) + the shell-readable `vbat` thermal zone (live voltage × 2 cells), with a shell PARTIAL wake lock via `IPowerManager`. Agrees with the app sampler within 0.1–0.3% |
| 5 | The background service cannot become foreground | MIUI default `RUN_ANY_IN_BACKGROUND=ignore` → "startForeground() not allowed due to bg restriction" | `cmd appops set … RUN_ANY_IN_BACKGROUND allow` + battery-optimisation exemption |
| 6 | A woken screen turns off after 10 s | with the swipe keyguard showing, `activityTimeoutWM=10000` | `wm dismiss-keyguard` |
| 7 | adb, install, input | developer options "Install via USB" and "USB debugging (Security settings)" are required. On Windows the libusbK driver needs `ADB_LIBUSB=1` | user settings + wrapper |

Do the 6 device-specific problems found on the C55 also show up on the Mix 4?

| C55 problem | Mix 4 |
|---|---|
| Identity CCM in capture results | **reproduced** — CCM from ForwardMatrix (D65) via `ccm_source forward_matrix` (existing code handles it) |
| Noise profile | plausible (a=2.4e-3, b=1.8e-5 @ ISO 1600) → HAL values used. Not bogus like the AVD |
| Sensor orientation | 90° (same as C55), handled by the existing upright rotation. CFA differs: Mix 4 **GRBG**, C55 GBRG — handled via meta |
| GPU delegate thread affinity | not reproduced (the dedicated owner thread is already there). App GPU segmentation init 1.7 s, inference 8.6 ms |
| Vendor library namespace (`uses-native-library`) | works with the same manifest (`uses_libraries=libOpenCL.so:libcdsprpc.so` in the load log) |
| perf restrictions (Samsung user build: no kernel samples or software events) | **none** — `perf_event_paranoid=-1`, `cpu-cycles:k` and `task-clock` both allowed |

Also, the Mix 4's ISO ceiling is 1600 (C55: 6400), so low-light bursts come out darker (same exposure policy). App GPU segmentation, bokeh and JPEG work end to end.

## 7. Limitations
- This is **whole-device (battery) power**. There is no per-rail (SoC/DDR/camera) breakdown, so "NPU" numbers mean "how much more the whole phone uses when running on the NPU", not NPU chip power. Idle subtraction removes the display, modem and resident services, but DRAM, L3 and power-rail (PMIC) efficiency are counted with the work.
- The fuel gauge updates every 250 ms and the BatteryManager current is the gauge's internal average → only **averages over ≥30 s of repeated work** are meaningful. Single events (one 2 ms inference, one shutter press) are not measured directly; they are repeated averages divided by count.
- Voltage: the app sampler used for the C55 and Mix 4 E1 (r1–r2) uses the broadcast voltage (updated minutes apart), so full-load energy is **overestimated by 2.3%** (measured concurrently on the Mix 4). The rest of the Mix 4 uses live `vbat`. Differences of a few % between the two phones are not interpreted.
- 30 s of continuous load **includes throttling** (C55 big cores drop 2361→2054 MHz within 30 s). A single cold shutter press will cost less than the tables show (estimate).
- In app experiments with the camera preview on, the hottest CPU sensor swings several °C within a second, so the "start + 1 °C" cool-down effectively ran to its cap (2–15 min). Alternating order and subtracting the idle right before reduce the bias, but stages did not run at identical temperatures.
- Runs: E1, E2, E4 ×3, **E3 ×2** (reduced for battery and time). Mix 4 E2 3-mid and X1-alone have 2 runs because r3 was excluded for background activity.
- Battery level: C55 E1 started at 88% (within 30–90%); because of the order, E3 ran at the lowest levels (C55 52→40%, Mix 4 42→**25%**) — the last Mix 4 E3 run (r2 off) was below the 30% floor. The voltage change with level (4.3→3.9 V) is accounted for by P = V × I, but gauge error may differ across the level range.
- The Mix 4 results are with MIUI capping the big cores at 2.11/2.15 GHz. Performance mode may differ (not measured).
- The C55 E3 governor effect (−1%) is within run-to-run spread. This data cannot support "the governor saves energy".

## 8. Reproducing

```bash
# build (cli NEON / scalar, app)
NDK=... bash scripts/build_arm64.sh && NO_NEON=1 NDK=... bash scripts/build_arm64.sh
QAIRT_SDK=... bash scripts/fetch_qnn.sh          # v68 + v69 Stub/Skel
cd android && ./gradlew assembleDebug && cd ..
# with the cable connected, set up wireless adb → unplug the cable
adb tcpip 5555 && adb connect <phone-ip>:5555
# validate the sampler (unit, sign, update period) → results/power/<dev>/calibration.json
python tools/power_analyze.py validate --csv samples.csv --records <val dir> --out results/power/<dev>/calibration.json
# cli experiments (E1 segmentation, E2 core placement). On MIUI use SAMPLER=shell
ADB="adb -s <ip>:5555" bash scripts/power_cli.sh c55 E1 3
ADB="adb -s <ip>:5555" SAMPLER=shell bash scripts/power_cli.sh mix4 E2 3
# INT8 NPU on the Mix 4 (needs LiteRT 1.4.2 .so pushed to /data/local/tmp/bp/l142)
ADB="adb -s <ip>:5555" SAMPLER=shell ONLY=npu MODEL=selfie_segmenter_int8.tflite LIBDIR=/data/local/tmp/bp/l142 bash scripts/power_cli.sh mix4 E1 3
# app experiments (E4 per stage, E3 150 shots)
ADB="adb -s <ip>:5555" DELEGATE=npu COOL_MAX=120 bash scripts/power_app.sh c55 E4 3
ADB="adb -s <ip>:5555" DELEGATE=gpu SAMPLER=shell bash scripts/power_app.sh mix4 E3 2
# tables and detail
python tools/power_report.py c55 mix4
python tools/power_e3.py results/power/<dev>/raw/<E3 run> --calib results/power/<dev>/calibration.json
```
cli: `--seconds S` (repeat for ≥S s after one warm-up), `--seg-only`, `--idle S`, `--power-json w.json --label L`.
App: `--es action pstage --es stage idle|capture|plain|portrait|seg|jpeg --ei seconds 30 --es label L`, `--es action stress --ei count 150 --es policy off|thermal --ez noseg true --es label L`.

## Summary for sharing

```
[Conditions]
C55 : Galaxy C55 SM-C5560, Snapdragon 7 Gen 1 (SM7450, HTP v69), Android 16 / One UI, 1 cell ~4.3 V.
      Cable unplugged, airplane mode + Wi-Fi (wireless adb), battery 88→40%, cli with screen off / app with window brightness 0.4 and preview on.
      Sampler: app PowerService (BatteryManager CURRENT_NOW, µA, discharge negative, gauge update 250 ms, 50 ms samples).
Mix4: Xiaomi Mi Mix 4, Snapdragon 888+ (SM8350, HTP v68), Android 14 / HyperOS 1.0, 2 cells ~8.6 V.
      Same conditions, battery 90→25% (only the last E3 run below 30%). MIUI caps big cores at 2.11/2.15 GHz (1.56 GHz while the camera is open).
      Sampler: shell ShellSampler (app_process, µA, discharge positive, live vbat × 2 cells) — within 0.1–0.3% of the app sampler.
All numbers = whole device (battery), net of a 30 s same-condition idle, ≥30 s repeated windows, median (3 runs; E3 2 runs).

[E1 net per segmentation inference — selfie_segmenter 256², same input]
              NPU              GPU              CPU (XNNPACK 4T)
C55        4.43 mJ / 1.99 ms  8.31 mJ / 16.0 ms  40.9 mJ / 9.2 ms
Mix4       fp16 impossible (v68 → CPU fallback 33.6 mJ)  7.55 mJ / 16.5 ms  33.3 mJ / 7.6 ms
           INT8 3.88 mJ / 4.15 ms (136/136 nodes, LiteRT 1.4.2; mask accuracy varies by scene — portrait IoU 0.86)

[E2 net per merge burst (4080×3072×8) — same C55 RAW input]
              scalar 1T   NEON little4   NEON top-core 1   NEON big4      NEON unpinned8
C55        2.61 J      2.19 J/1723ms   1.81 J/932ms      1.67 J/318ms*  1.72 J/295ms
Mix4       2.29 J      1.68 J/2104ms   1.52 J/795ms*     1.62 J/258ms   1.61 J/246ms
(* lowest. C55 = race-to-idle (big cores; little +31%), Mix4 = X1 alone (placements within ±5%; little +10%). NEON vs scalar energy −31/−34%)

[E3 150 consecutive shots, net J/shot (shutter→JPEG ms), segmentation and bokeh off]
              governor off       governor on
C55        2.13 J (1282)      2.11 J (1277)   throttle step 1 at 7–9 s, step 2 at 86–99 s (shots 68–79) → processing 430→524 ms; governor N 8→7.3 from shot 121, last 30 shots −41 ms
Mix4       2.33 J (1282)      2.34 J (1277)   OS fixes 1.56 GHz from the start while the camera is open → processing constant at 387 ms, governor never acts
→ On both phones the energy difference between policies is within run-to-run spread (±1%).

[E4 one portrait shot, net per stage (relative to preview)]  C55: capture 0.10 + processing 1.53 + JPEG 0.18 ≈ 1.8 J, keeping preview 2.76 W (≈3.3 J/shot)
                                                             Mix4: capture 0.58 + processing 1.51 + JPEG 0.18 ≈ 2.3 J, keeping preview 3.14 W
→ Per shot, keeping the preview running costs more of the device's energy than the processing.

[Mix 4 porting issues]
1. HTP v68 has no fp16 → all 246 ops fail validation; the delegate says "ok" but runs on the CPU (confirmed by logs and energy).
   INT8 a8w8 model (calibrated on real bursts) + LiteRT 1.4.2 → 136/136 nodes on the NPU, 3.88 mJ (half the GPU). The device's LiteRT 2.16 cannot load it. Open: per-tensor INT8 accuracy (SE gating path)
2. RAW is 27 MP (6016×4512) only → 8 slots exceed the Java heap (OOM) → centre-crop to 4080×3072 (capture energy stays 6× the C55)
3. Slot reallocation OOM on back-to-back capture (also on the C55) → reuse slots
4. MIUI AutoPowerKill stops even a foreground service 7–16 min after screen-off → shell-uid sampler (app_process + IBatteryPropertiesRegistrar + IPowerManager wake lock)
5. MIUI background restriction (startForeground refused) → appops RUN_ANY_IN_BACKGROUND / swipe keyguard 10 s timeout → dismiss-keyguard
6. C55 device issues on the Mix 4: identity CCM reproduced (handled via ForwardMatrix); noise profile, orientation, GPU thread, namespace fine; no perf restrictions (paranoid −1). CFA GRBG, ISO ceiling 1600

[One-line resume candidates]
- Built a battery-current sampler and idle-subtracted measurement setup on two Snapdragon phones (7 Gen 1 / 888+) and measured per-stage energy of a RAW burst HDR + portrait pipeline (segmentation NPU 4.4 mJ vs GPU 8.3 mJ vs CPU 41 mJ).
- Measured merge energy per core placement and showed race-to-idle depends on the SoC: on 7 Gen 1 big cores use 24% less than little cores; on 888+ the X1 alone is lowest and placements are within ±5%.
- While porting to the Mi Mix 4, found from delegated-node counts and energy that HTP v68's lack of fp16 made the NPU silently fall back to the CPU (33.6 mJ), and moved all 136 nodes onto the NPU with INT8 quantisation (3.9 mJ, 8.7× less). Also diagnosed and worked around a 27 MP RAW OOM and MIUI AutoPowerKill.
```
