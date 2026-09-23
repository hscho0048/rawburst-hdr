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
    private var stressTotal = 0

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
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

    override fun onNewIntent(intent: Intent) { super.onNewIntent(intent); takeAction(intent) }

    private fun takeAction(i: Intent?) {
        pendingAction = i?.getStringExtra("action") ?: return
        pendingDir = i.getStringExtra("dir")
        // stress: 연속 촬영 N회 (설계문서 L7). policy: "adpf,thermal" / "off"
        stressTotal = i.getIntExtra("count", 30)
        pendingPolicy = i.getStringExtra("policy")
        runPendingWhenReady()
    }

    private fun runPendingWhenReady() {
        val a = pendingAction ?: return
        val needCam = a != "emu"
        if ((needCam && !(cam.ready && nativeReady)) || (!needCam && !nativeReady && cam.cameraId != null)) {
            window.decorView.postDelayed({ runPendingWhenReady() }, 300); return
        }
        pendingAction = null
        Log.i(CaptureController.TAG, "action $a (camReady=${cam.ready} native=$nativeReady)")
        when (a) {
            "shoot" -> doShoot(false); "dump" -> doShoot(true); "emu" -> doEmu(pendingDir)
            "stress" -> {
                val pol = pendingPolicy ?: "off"
                procHandler.post {
                    val st = Native.setPolicy(if (pol.contains("adpf")) 400 else 0, pol.contains("thermal"))
                    Log.i(CaptureController.TAG, "STRESS start count=$stressTotal policy=$pol $st")
                    stressLeft = stressTotal
                    runOnUiThread { doShoot(false) }
                }
            }
        }
    }

    private fun openCamera(h: SurfaceHolder) {
        cam.start(h.surface)
        if (nativeReady || cam.rawSize.width == 0) return
        procHandler.post {
            // 세그 모델이 assets에 있으면 사용 (LiteRT 포함 빌드에서만 성공). 없으면 보케 생략.
            val model = runCatching {
                File(filesDir, "selfie_segmenter.tflite").also { f ->
                    if (!f.exists()) assets.open("selfie_segmenter.tflite").use { src -> f.outputStream().use { src.copyTo(it) } }
                }.absolutePath
            }.getOrNull()
            val cores = Runtime.getRuntime().availableProcessors()
            val cpus = if (cores >= 8) intArrayOf(4, 5, 6, 7) else IntArray(0)   // SM7450: 4~7 = A710
            val threads = minOf(4, cores)
            val st = Native.init(cam.rawSize.width, cam.rawSize.height, cam.n, model, 1 /*gpu*/, threads, cpus)
            nativeReady = true
            setStatus("${cam.describe()}\nnative: threads=$threads cpus=${cpus.joinToString(",")} $st")
        }
    }

    private fun doShoot(dump: Boolean) {
        if (!cam.ready || !nativeReady) { setStatus("카메라 준비 중"); return }
        shutterAt = SystemClock.elapsedRealtime()
        if (!cam.shoot(dump)) { Log.w(CaptureController.TAG, "shoot rejected (busy)"); setStatus("이전 버스트 진행 중") }
        else setStatus("burst ${if (dump) "(dump)" else ""}…")
    }

    override fun onBurst(d: BurstDumper) {
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
