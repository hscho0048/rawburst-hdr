package dev.burstpipe

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.*
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import java.util.concurrent.Executor
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Camera2 RAW 버스트 캡처. 스트림 2개(프리뷰 PRIV + RAW_SENSOR MAXIMUM).
 * HDR+ 노출 정책: 버스트 전 프레임 같은 노출, EV −1.5, 노출시간 상한 1/30s, 부족분은 ISO.
 *   MANUAL_SENSOR 있으면 AE_MODE_OFF + 수동값, 없으면(LIMITED) AE_LOCK + 노출 보정.
 */
class CaptureController(ctx: Context, private val listener: Listener) {
    interface Listener {
        fun onBurst(d: BurstDumper)
        fun onError(msg: String)
    }

    val n = 8
    private val appCtx = ctx.applicationContext
    private val mgr = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager

    /** 후면 + RAW 우선, 없으면 RAW 되는 아무 카메라. 없으면 null (설계문서 2장: YUV 폴백은 범위 밖) */
    val cameraId: String? = run {
        val raw = mgr.cameraIdList.filter { id ->
            mgr.getCameraCharacteristics(id).get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
                ?.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_RAW) == true
        }
        raw.firstOrNull {
            mgr.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        } ?: raw.firstOrNull()
    }
    val chars: CameraCharacteristics? = cameraId?.let { mgr.getCameraCharacteristics(it) }
    val rawSize: Size = chars?.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        ?.getOutputSizes(ImageFormat.RAW_SENSOR)?.maxByOrNull { it.width * it.height } ?: Size(0, 0)
    val hasManual: Boolean = chars?.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
        ?.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) == true

    /** 프리뷰 크기: 1280×720 근처 */
    val previewSize: Size = chars?.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        ?.getOutputSizes(SurfaceHolder::class.java)
        ?.minByOrNull { abs(it.width * it.height - 1280 * 720) } ?: Size(1280, 720)

    private val thread = HandlerThread("cam").apply { start() }
    private val handler = Handler(thread.looper)
    private val executor = Executor { handler.post(it) }
    private val rawReader: ImageReader? =
        if (rawSize.width > 0) ImageReader.newInstance(rawSize.width, rawSize.height, ImageFormat.RAW_SENSOR, n + 2) else null
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private lateinit var previewSurface: Surface
    @Volatile private var lastExposureNs = 16_666_666L
    @Volatile private var lastIso = 400
    @Volatile var ready = false
        private set
    @Volatile private var current: BurstDumper? = null

    fun describe(): String = "camera=$cameraId raw=${rawSize.width}x${rawSize.height} manual=$hasManual " +
        "level=${chars?.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL)} " +
        "cfa=${chars?.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT)}"

    fun start(preview: Surface) {
        val id = cameraId ?: return listener.onError("RAW 지원 카메라 없음 (설계문서 2장 YUV 폴백 필요)")
        previewSurface = preview
        // 이미지 콜백: 도착 즉시 슬롯으로 복사하고 close — HAL 버퍼를 오래 잡으면 프레임 드롭
        rawReader!!.setOnImageAvailableListener({ r ->
            r.acquireNextImage()?.let { img -> current?.onImage(img); img.close() }
        }, handler)
        try {
            mgr.openCamera(id, object : CameraDevice.StateCallback() {
                override fun onOpened(d: CameraDevice) { device = d; createSession(d) }
                override fun onDisconnected(d: CameraDevice) { d.close(); ready = false }
                override fun onError(d: CameraDevice, e: Int) { d.close(); ready = false; listener.onError("camera error $e") }
            }, handler)
        } catch (e: SecurityException) {
            listener.onError("카메라 권한 없음")
        }
    }

    fun stop() {
        ready = false
        session?.close(); session = null
        device?.close(); device = null
    }

    private fun createSession(d: CameraDevice) {
        val outputs = listOf(OutputConfiguration(previewSurface), OutputConfiguration(rawReader!!.surface))
        val cfg = SessionConfiguration(SessionConfiguration.SESSION_REGULAR, outputs, executor,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) { session = s; startPreview(d, s) }
                override fun onConfigureFailed(s: CameraCaptureSession) { listener.onError("session config failed") }
            })
        d.createCaptureSession(cfg)
    }

    private fun startPreview(d: CameraDevice, s: CameraCaptureSession) {
        val req = d.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply { addTarget(previewSurface) }
        s.setRepeatingRequest(req.build(), object : CameraCaptureSession.CaptureCallback() {
            override fun onCaptureCompleted(s: CameraCaptureSession, r: CaptureRequest, res: TotalCaptureResult) {
                res.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { lastExposureNs = it }
                res.get(CaptureResult.SENSOR_SENSITIVITY)?.let { lastIso = it }
                ready = true
            }
        }, handler)
    }

    /** 버스트 1회. dumpToDisk=true면 raw16 + meta.txt 를 files/burst_<ts>/ 에 쓴다 (PC 파이프라인 입력). */
    fun shoot(dumpToDisk: Boolean): Boolean {
        val d = device ?: return false
        val s = session ?: return false
        val c = chars ?: return false
        if (current != null) return false  // 이전 버스트 진행 중
        val dumper = BurstDumper(appCtx, rawSize.width, rawSize.height, n, c, dumpToDisk) { done ->
            current = null
            listener.onBurst(done)
        }
        current = dumper

        val req = d.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE).apply {
            addTarget(rawReader!!.surface)
            set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_OFF)
            if (hasManual) {
                var expo = (lastExposureNs / 2.83).toLong()   // EV −1.5
                var iso = lastIso
                val maxExpo = 33_333_333L                        // 1/30s (손떨림)
                if (expo > maxExpo) { iso = (iso.toLong() * expo / maxExpo).toInt(); expo = maxExpo }
                c.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)?.let { expo = expo.coerceIn(it.lower, it.upper) }
                c.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)?.let { iso = iso.coerceIn(it.lower, it.upper) }
                set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                set(CaptureRequest.SENSOR_EXPOSURE_TIME, expo)
                set(CaptureRequest.SENSOR_SENSITIVITY, iso)
                Log.i(TAG, "burst manual expo=${expo}ns iso=$iso (preview ${lastExposureNs}ns/$lastIso)")
            } else {
                val step = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toFloat() ?: (1f / 3f)
                val range = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
                var comp = (-1.5f / step).roundToInt()
                if (range != null) comp = comp.coerceIn(range.lower, range.upper)
                set(CaptureRequest.CONTROL_AE_LOCK, true)
                set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, comp)
                Log.i(TAG, "burst AE lock comp=$comp")
            }
        }.build()
        s.captureBurst(List(n) { req }, object : CameraCaptureSession.CaptureCallback() {
            override fun onCaptureCompleted(s: CameraCaptureSession, r: CaptureRequest, res: TotalCaptureResult) {
                dumper.onResult(res)
            }
            override fun onCaptureFailed(s: CameraCaptureSession, r: CaptureRequest, f: CaptureFailure) {
                Log.w(TAG, "capture failed reason=${f.reason}")
                dumper.onFailed()
            }
        }, handler)
        // 결과/이미지가 모자라도 2초 뒤에는 있는 것으로 마무리 (프레임 드롭 대비)
        handler.postDelayed({ dumper.finishIfPending() }, 2000)
        return true
    }

    companion object { const val TAG = "burstpipe" }
}
