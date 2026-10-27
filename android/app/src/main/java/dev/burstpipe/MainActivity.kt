package dev.burstpipe

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.nio.ByteBuffer

/**
 * SHOOT: 버스트 → JNI 파이프라인 → 결과 표시 + JPEG. 셔터→JPEG wall time 표시.
 * DUMP : 버스트를 files/burst_<ts>/ 에 raw16 + meta.txt로 저장 (PC cli 입력). 처리도 함께 한다.
 * EMU  : files/emu_xxx 디렉터리 (adb push한 에뮬 버스트)를 처리 — 기기 없이 JNI·보케 경로 검증.
 * 자동화: adb shell am start -n dev.burstpipe/.MainActivity --es action shoot|dump|emu [--es dir <path>]
 * 전력 실험 (docs/power.md, 샘플은 PowerService가 따로 남긴다. 여기는 구간 경계 boottime ns + 반복 기록):
 *   --es action stress --ei count 150 --es policy off|thermal --es label L   → files/power/L.json  (E3)
 *   --es action pstage --es stage idle|capture|plain|portrait|seg|jpeg --ei seconds 30 --es label L     (E4)
 *   --ef brightness 0.5 : 창 밝기 고정 (앱 실험 공통 조건)
 */
class MainActivity : Activity(), CaptureController.Listener {
    private lateinit var cam: CaptureController
    private lateinit var status: TextView
    private lateinit var result: ImageView
    private val proc = HandlerThread("proc").apply { start() }
    private val procHandler = Handler(proc.looper)
    @Volatile private var nativeReady = false
    @Volatile private var shutterAt = 0L
    private var pendingAction: String? = null
    private var pendingDir: String? = null
    private var stressLeft = 0
    private var pendingPolicy: String? = null
    @Volatile private var ndkMode = false
    private var stressTotal = 0
    // 전력 실험 기록
    private var pLabel: String? = null
    private var pT0 = 0L
    private val pShots = StringBuilder()
    private val pIterMs = ArrayList<Double>()
    @Volatile private var captureOnly = false
    @Volatile private var holdBurst: ((BurstDumper) -> Unit)? = null
    private var pSeconds = 30

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        // 무인 실험: 잠금 없이 화면을 켜고 유지 (잠금화면 자격증명은 쓰지 않는다)
        setShowWhenLocked(true); setTurnScreenOn(true)
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        applyBrightness(intent)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val sv = SurfaceView(this)
        result = ImageView(this).apply { scaleType = ImageView.ScaleType.FIT_CENTER }
        status = TextView(this).apply { textSize = 11f; setPadding(12, 6, 12, 6) }
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val shoot = Button(this).apply { text = "SHOOT" }
        val dump = Button(this).apply { text = "DUMP" }
        val emu = Button(this).apply { text = "EMU BURST" }
        for (bt in listOf(shoot, dump, emu)) row.addView(bt, LinearLayout.LayoutParams(0, -2, 1f))
        root.addView(sv, LinearLayout.LayoutParams(-1, 0, 1f))
        root.addView(result, LinearLayout.LayoutParams(-1, 0, 1f))
        root.addView(status, LinearLayout.LayoutParams(-1, -2))
        root.addView(row, LinearLayout.LayoutParams(-1, -2))
        setContentView(root)

        cam = CaptureController(this, this)
        ndkMode = intent?.getStringExtra("capture") == "ndk"  // Camera2 NDK 캡처 (C++) 경로
        setStatus(cam.describe())
        shoot.setOnClickListener { doShoot(false) }
        dump.setOnClickListener { doShoot(true) }
        emu.setOnClickListener { doEmu(null) }
        takeAction(intent)

        sv.holder.setFixedSize(cam.previewSize.width, cam.previewSize.height)
        sv.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) { if (hasCamPerm()) openCamera(h) else requestPermissions(arrayOf(Manifest.permission.CAMERA), 1) }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {}
            override fun surfaceDestroyed(h: SurfaceHolder) { cam.stop() }
        })
        this.sv = sv
    }

    private var sv: SurfaceView? = null
    private fun hasCamPerm() = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (hasCamPerm()) sv?.holder?.let { if (it.surface.isValid) openCamera(it) } else setStatus("카메라 권한 거부됨")
    }

    override fun onNewIntent(intent: Intent) { super.onNewIntent(intent); setIntent(intent); applyBrightness(intent); takeAction(intent) }

    private fun applyBrightness(i: Intent?) {
        val v = i?.getFloatExtra("brightness", -1f) ?: -1f
        if (v >= 0f) window.attributes = window.attributes.apply { screenBrightness = v }
    }

    private fun takeAction(i: Intent?) {
        pendingAction = i?.getStringExtra("action") ?: return
        pendingDir = i.getStringExtra("dir")
        // stress: 연속 촬영 N회 (설계문서 L7). policy: "adpf,thermal" / "off"
        stressTotal = i.getIntExtra("count", 30)
        pendingPolicy = i.getStringExtra("policy")
        pLabel = i.getStringExtra("label")
        pSeconds = i.getIntExtra("seconds", 30)
        pendingStage = i.getStringExtra("stage")
        runPendingWhenReady()
    }

    private fun runPendingWhenReady() {
        val a = pendingAction ?: return
        val needCam = a != "emu"
        val camReady = if (ndkMode) nativeReady else cam.ready
        if ((needCam && !(camReady && nativeReady)) || (!needCam && !nativeReady && cam.cameraId != null)) {
            window.decorView.postDelayed({ runPendingWhenReady() }, 300); return
        }
        pendingAction = null
        // --ez noseg true: 세그·보케 없이 (E3 연속 촬영을 기존 150연속 측정과 같은 조건으로)
        val noseg = intent?.getBooleanExtra("noseg", false) == true
        procHandler.post { Native.setSegEnabled(!noseg) }
        Log.i(CaptureController.TAG, "action $a (camReady=${cam.ready} native=$nativeReady noseg=$noseg)")
        when (a) {
            "shoot" -> doShoot(false); "dump" -> doShoot(true); "emu" -> doEmu(pendingDir)
            "stress" -> {
                val pol = pendingPolicy ?: "off"
                procHandler.post {
                    val st = Native.setPolicy(if (pol.contains("adpf")) 400 else 0, pol.contains("thermal"))
                    Log.i(CaptureController.TAG, "STRESS start count=$stressTotal policy=$pol $st")
                    pShots.setLength(0); pT0 = SystemClock.elapsedRealtimeNanos()
                    stressLeft = stressTotal
                    runOnUiThread { doShoot(false) }
                }
            }
            "pstage" -> runStage(pendingStage ?: "idle")
        }
    }

    private var pendingStage: String? = null

    /** 전력 구간 기록 → files/power/<label>.json (tmp에 쓰고 rename: 파일이 보이면 완료) */
    private fun writeRecord(mode: String, t0: Long, t1: Long, iters: Int, extra: String) {
        val label = pLabel ?: "${mode}_${System.currentTimeMillis()}"
        val dir = File(getExternalFilesDir(null), "power").apply { mkdirs() }
        val ms = pIterMs.sorted()
        val med = if (ms.isEmpty()) 0.0 else ms[ms.size / 2]
        val sec = (t1 - t0) / 1e9
        val js = "{\"label\":\"$label\",\"mode\":\"$mode\",\"t0_ns\":$t0,\"t1_ns\":$t1,\"seconds\":$sec,\"iters\":$iters," +
            "\"ms_per_iter\":${if (iters > 0) sec * 1e3 / iters else 0.0},\"iter_ms_median\":$med,$extra}"
        val tmp = File(dir, "$label.json.tmp"); tmp.writeText(js + "\n"); tmp.renameTo(File(dir, "$label.json"))
        Log.i(CaptureController.TAG, "POWER record $label iters=$iters sec=$sec median=$med")
    }

    /**
     * E4: 단계 하나를 seconds 이상 반복. 카메라 프리뷰는 계속 켜져 있다 → idle(프리뷰만) 구간과 차분하면 그 단계의 순 에너지.
     *  capture: RAW 버스트만 (처리 안 함) / plain: 같은 버스트 합성+마무리(세그·보케 끔) / portrait: 합성+세그+보케
     *  seg: 세그 전처리+추론만 / jpeg: Bitmap + JPEG(95) 파일 쓰기만
     */
    private fun runStage(stage: String) {
        pIterMs.clear()
        Log.i(CaptureController.TAG, "PSTAGE $stage ${pSeconds}s label=$pLabel")
        when (stage) {
            "idle" -> {
                val t0 = SystemClock.elapsedRealtimeNanos()
                window.decorView.postDelayed({ writeRecord("app_idle", t0, SystemClock.elapsedRealtimeNanos(), 0, "\"stage\":\"idle\"") }, pSeconds * 1000L)
            }
            "capture" -> {
                captureOnly = true
                pT0 = SystemClock.elapsedRealtimeNanos()
                doShoot(false)
            }
            else -> {
                // 입력 버스트 1장 확보 (구간 밖) → 처리 스레드에서 반복
                holdBurst = { d -> procHandler.post { stageLoop(stage, d) } }
                doShoot(false)
            }
        }
    }

    private fun stageLoop(stage: String, d: BurstDumper) {
        val out = ByteBuffer.allocateDirect(d.width * d.height * 4)
        Native.setSegEnabled(stage != "plain")
        val first = Native.process(d.slots, d.count, d.metaText, out)  // 웜업 + jpeg 입력
        val t0 = SystemClock.elapsedRealtimeNanos()
        var iters = 0
        var lastJson = first
        do {
            val a = SystemClock.elapsedRealtimeNanos()
            when (stage) {
                "plain", "portrait" -> lastJson = Native.process(d.slots, d.count, d.metaText, out)
                "seg" -> lastJson = Native.segOnly(d.slots[0], d.metaText)
                "jpeg" -> {
                    val bmp = Bitmap.createBitmap(d.width, d.height, Bitmap.Config.ARGB_8888).apply { out.rewind(); copyPixelsFromBuffer(out) }
                    File(getExternalFilesDir(null), "pstage_jpeg.jpg").outputStream().use { bmp.compress(Bitmap.CompressFormat.JPEG, 95, it) }
                    bmp.recycle()
                }
            }
            pIterMs.add((SystemClock.elapsedRealtimeNanos() - a) / 1e6)
            iters++
        } while (SystemClock.elapsedRealtimeNanos() - t0 < pSeconds * 1_000_000_000L)
        val t1 = SystemClock.elapsedRealtimeNanos()
        Native.setSegEnabled(true)
        writeRecord("app_$stage", t0, t1, iters, "\"stage\":\"$stage\",\"frames\":${d.count},\"last\":${lastJson.ifEmpty { "null" }}")
    }

    private fun openCamera(h: SurfaceHolder) {
        if (!ndkMode) cam.start(h.surface)
        if (nativeReady || cam.rawSize.width == 0) return
        procHandler.post {
            // 세그 모델이 assets에 있으면 사용 (LiteRT 포함 빌드에서만 성공). 없으면 보케 생략.
            // _std = 커스텀 op를 표준 TRANSPOSE_CONV로 바꾼 모델 (tools/convert_selfie_model.py): NPU가 그래프 전체를 가져간다
            val model = listOf("selfie_segmenter_std.tflite", "selfie_segmenter.tflite").firstNotNullOfOrNull { name ->
                runCatching {
                    File(filesDir, name).also { f ->
                        if (!f.exists()) assets.open(name).use { src -> f.outputStream().use { src.copyTo(it) } }
                    }.absolutePath
                }.getOrNull()
            }
            // 델리게이트: 인텐트 --es delegate gpu|npu|cpu (기본 gpu). NPU는 skel을 nativeLibraryDir에서 찾게 한다
            val delegate = when (intent?.getStringExtra("delegate")) { "npu" -> 2; "cpu" -> 0; else -> 1 }
            if (delegate == 2) android.system.Os.setenv("ADSP_LIBRARY_PATH", applicationInfo.nativeLibraryDir + ";/vendor/dsp/cdsp;/vendor/lib/rfsa/adsp", true)
            val cores = Runtime.getRuntime().availableProcessors()
            val cpus = if (cores >= 8) intArrayOf(4, 5, 6, 7) else IntArray(0)   // SM7450: 4~7 = A710
            val threads = minOf(4, cores)
            val st = Native.init(cam.rawSize.width, cam.rawSize.height, cam.n, model, delegate, threads, cpus)
            val ndkInfo = if (ndkMode) Native.ndkOpen(h.surface, cam.n) else ""
            nativeReady = true
            setStatus("${cam.describe()}\nnative: threads=$threads cpus=${cpus.joinToString(",")} $st $ndkInfo")
        }
    }

    private fun doShoot(dump: Boolean) {
        if (ndkMode) { doShootNdk(); return }
        if (!cam.ready || !nativeReady) { setStatus("카메라 준비 중"); return }
        shutterAt = SystemClock.elapsedRealtime()
        if (!cam.shoot(dump)) { Log.w(CaptureController.TAG, "shoot rejected (busy)"); setStatus("이전 버스트 진행 중") }
        else setStatus("burst ${if (dump) "(dump)" else ""}…")
    }

    override fun onBurst(d: BurstDumper) {
        holdBurst?.let { h -> holdBurst = null; h(d); return }
        if (captureOnly) {  // E4 capture: 처리 없이 다음 버스트
            pIterMs.add((SystemClock.elapsedRealtime() - shutterAt).toDouble())
            if (SystemClock.elapsedRealtimeNanos() - pT0 < pSeconds * 1_000_000_000L) {
                window.decorView.post { doShoot(false) }
            } else {
                captureOnly = false
                writeRecord("app_capture", pT0, SystemClock.elapsedRealtimeNanos(), pIterMs.size, "\"stage\":\"capture\",\"frames\":${d.count}")
            }
            return
        }
        procHandler.post {
            val out = ByteBuffer.allocateDirect(d.width * d.height * 4)
            val t0 = SystemClock.elapsedRealtime()
            val json = Native.process(d.slots, d.count, d.metaText, out)
            val procMs = SystemClock.elapsedRealtime() - t0
            if (json.isEmpty()) { setStatus("process failed (frames=${d.count})"); return@post }
            val jpg = saveJpeg(out, d.width, d.height)
            val wall = SystemClock.elapsedRealtime() - shutterAt
            val msg = "shutter→jpeg ${wall}ms (capture ${d.captureMs} + process $procMs + jpeg) frames=${d.count}\n" +
                (d.dir?.let { "dump: $it\n" } ?: "") + "jpeg: ${jpg.name}\n$json"
            Log.i(CaptureController.TAG, "RESULT $msg")
            setStatus(msg)
            if (stressLeft > 0) {
                stressLeft--
                Log.i(CaptureController.TAG, "STRESS shot=${stressTotal - stressLeft} wall=$wall $json")
                if (pShots.isNotEmpty()) pShots.append(',')
                pShots.append("{\"t_ns\":${SystemClock.elapsedRealtimeNanos()},\"wall\":$wall,\"capture\":${d.captureMs},\"proc\":$procMs,\"t\":$json}")
                pIterMs.add(wall.toDouble())
                if (stressLeft > 0) window.decorView.postDelayed({ doShoot(false) }, 50) else {
                    Log.i(CaptureController.TAG, "STRESS done")
                    writeRecord("app_stress", pT0, SystemClock.elapsedRealtimeNanos(), stressTotal,
                        "\"policy\":\"${pendingPolicy ?: "off"}\",\"shots\":[$pShots]")
                    pIterMs.clear()
                }
            }
        }
    }

    /** NDK 경로: 캡처·메타·처리 전부 C++ (프레임이 Java를 거치지 않음) */
    private fun doShootNdk() {
        shutterAt = SystemClock.elapsedRealtime()
        procHandler.post {
            val w = cam.rawSize.width; val h = cam.rawSize.height
            val out = ByteBuffer.allocateDirect(w * h * 4)
            val json = Native.ndkShoot(out)
            if (json.isEmpty()) { setStatus("ndk shoot failed"); return@post }
            val jpg = saveJpeg(out, w, h)
            val wall = SystemClock.elapsedRealtime() - shutterAt
            val msg = "[ndk] shutter→jpeg ${wall}ms\njpeg: ${jpg.name}\n$json"
            Log.i(CaptureController.TAG, "RESULT $msg")
            setStatus(msg)
            if (stressLeft > 0) {
                stressLeft--
                Log.i(CaptureController.TAG, "STRESS shot=${stressTotal - stressLeft} wall=$wall $json")
                if (stressLeft > 0) window.decorView.postDelayed({ doShoot(false) }, 50) else Log.i(CaptureController.TAG, "STRESS done")
            }
        }
    }

    private fun doEmu(dirArg: String?) {
        procHandler.post {
            // adb push로 외부 앱 폴더에 넣은 파일은 scoped storage 때문에 앱이 못 읽는 경우가 있다 →
            // scripts/app_emu_test.sh 는 run-as로 내부 filesDir에 복사한다. 두 곳 모두 찾는다.
            val roots = listOfNotNull(filesDir, getExternalFilesDir(null))
            val dir = dirArg?.let { File(it) } ?: roots.flatMap { r -> r.listFiles()?.toList() ?: emptyList() }
                .filter { it.isDirectory && it.name.startsWith("emu_") && File(it, "meta.txt").canRead() }
                .maxByOrNull { it.lastModified() }
            if (dir == null || !File(dir, "meta.txt").canRead()) {
                val msg = "에뮬 버스트 없음/읽기 불가: ${dir ?: roots.joinToString()} — scripts/app_emu_test.sh 참고"
                Log.w(CaptureController.TAG, msg); setStatus(msg); return@post
            }
            if (!nativeReady) {  // 카메라 없이 EMU만 쓰는 경우
                Native.init(16, 16, 1, null, 0, minOf(4, Runtime.getRuntime().availableProcessors()), IntArray(0)); nativeReady = true
            }
            val kv = File(dir, "meta.txt").readLines().associate { l -> l.substringBefore(' ') to l.substringAfter(' ') }
            val w = kv["width"]!!.trim().toInt(); val h = kv["height"]!!.trim().toInt()
            val out = ByteBuffer.allocateDirect(w * h * 4)
            val json = Native.processDir(dir.absolutePath, out)
            if (json.isEmpty()) { setStatus("processDir failed: $dir"); return@post }
            val jpg = saveJpeg(out, w, h)
            val msg = "emu ${dir.name} ${w}x$h → ${jpg.name}\n$json"
            Log.i(CaptureController.TAG, "RESULT $msg")
            setStatus(msg)
        }
    }

    private fun saveJpeg(rgba: ByteBuffer, w: Int, h: Int): File {
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).apply { rgba.rewind(); copyPixelsFromBuffer(rgba) }
        val f = File(getExternalFilesDir(null), "result_${System.currentTimeMillis()}.jpg")
        f.outputStream().use { bmp.compress(Bitmap.CompressFormat.JPEG, 95, it) }
        runOnUiThread { result.setImageBitmap(bmp) }
        return f
    }

    private fun setStatus(s: String) = runOnUiThread { status.text = s }

    override fun onError(msg: String) { Log.e(CaptureController.TAG, msg); setStatus("error: $msg") }
}
