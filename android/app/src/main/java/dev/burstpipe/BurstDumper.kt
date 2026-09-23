package dev.burstpipe

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.media.Image
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * 버스트를 RAM 슬롯에 모은다. 이미지 콜백 안에서 디스크 I/O를 하면 HAL 버퍼가 밀려 프레임이 떨어진다.
 * 슬롯은 direct ByteBuffer — JNI(Native.process)에 복사 없이 넘긴다.
 * 프레임↔메타 매칭은 SENSOR_TIMESTAMP == Image.timestamp (순서 가정 금지).
 */
class BurstDumper(
    private val ctx: Context, val width: Int, val height: Int, val n: Int,
    private val chars: CameraCharacteristics, private val dumpToDisk: Boolean,
    private val onDone: (BurstDumper) -> Unit,
) {
    val slots: Array<ByteBuffer> = Array(n) { ByteBuffer.allocateDirect(width * height * 2).order(ByteOrder.nativeOrder()) }
    private val timestamps = LongArray(n)
    var count = 0
        private set
    private var failed = 0
    private val results = HashMap<Long, TotalCaptureResult>()
    private var finished = false
    var dir: File? = null
        private set
    var metaText: String = ""
        private set
    var captureMs = 0L
        private set
    private val t0 = android.os.SystemClock.elapsedRealtime()

    @Synchronized fun onImage(img: Image) {
        if (count >= n || finished) return
        val plane = img.planes[0]
        val src = plane.buffer
        val rowStride = plane.rowStride
        val dst = slots[count]
        dst.clear()
        val rowBytes = width * 2
        if (rowStride == rowBytes) {
            dst.put(src)
        } else {
            val row = ByteArray(rowBytes)
            for (y in 0 until height) { src.position(y * rowStride); src.get(row, 0, rowBytes); dst.put(row) }
        }
        dst.flip()
        timestamps[count] = img.timestamp
        count++
        maybeFinish()
    }

    @Synchronized fun onResult(res: TotalCaptureResult) {
        res.get(CaptureResult.SENSOR_TIMESTAMP)?.let { results[it] = res }
        maybeFinish()
    }

    @Synchronized fun onFailed() { failed++; maybeFinish() }

    @Synchronized fun finishIfPending() { if (!finished && count > 0) finish() }

    private fun maybeFinish() {
        if (!finished && count + failed >= n && results.size >= count) finish()
    }

    private fun finish() {
        finished = true
        captureMs = android.os.SystemClock.elapsedRealtime() - t0
        // 타임스탬프 순으로 슬롯 정렬 (도착 순서 ≠ 촬영 순서일 수 있음)
        val order = (0 until count).sortedBy { timestamps[it] }
        val sortedSlots = order.map { slots[it] }
        val sortedTs = order.map { timestamps[it] }
        for (i in 0 until count) slots[i] = sortedSlots[i]
        for (i in 0 until count) timestamps[i] = sortedTs[i]
        metaText = buildMeta()
        if (dumpToDisk) writeDisk()
        Log.i(CaptureController.TAG, "burst done frames=$count failed=$failed results=${results.size} capture=${captureMs}ms dir=$dir")
        onDone(this)
    }

    private fun buildMeta(): String {
        val first = results[timestamps[0]] ?: results.values.firstOrNull()
        val sb = StringBuilder()
        sb.append("width $width\nheight $height\n")
        sb.append("cfa ${chars.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT) ?: 0}\n")
        sb.append("white_level ${chars.get(CameraCharacteristics.SENSOR_INFO_WHITE_LEVEL) ?: 1023}\n")
        // 동적 블랙레벨(결과)이 있으면 우선, 없으면 정적 패턴. 순서: (0,0) (1,0) (0,1) (1,1) = (y&1)*2+(x&1)
        val dyn = first?.get(CaptureResult.SENSOR_DYNAMIC_BLACK_LEVEL)
        val bl = if (dyn != null) dyn.map { it.toDouble() } else {
            val a = IntArray(4); chars.get(CameraCharacteristics.SENSOR_BLACK_LEVEL_PATTERN)?.copyTo(a, 0); a.map { it.toDouble() }
        }
        sb.append("black_level ${bl.joinToString(" ")}\n")
        first?.get(CaptureResult.COLOR_CORRECTION_GAINS)?.let {
            sb.append("wb_gains ${it.red} ${it.greenEven} ${it.greenOdd} ${it.blue}\n")
        }
        // CCM: 결과의 COLOR_CORRECTION_TRANSFORM이 단위행렬/없음이면 (Galaxy C55 HAL이 그렇다) DNG 방식으로 계산:
        //   WB 후 카메라 RGB → XYZ(D50) = ForwardMatrix(D65 쪽), XYZ(D50) → 선형 sRGB(D65, Bradford)
        val resCcm = first?.get(CaptureResult.COLOR_CORRECTION_TRANSFORM)?.let { m ->
            (0 until 3).flatMap { r -> (0 until 3).map { c -> m.getElement(c, r).toDouble() } }  // 행 우선
        }
        val isIdentity = resCcm == null || resCcm.withIndex().all { (i, v) -> kotlin.math.abs(v - if (i % 4 == 0) 1.0 else 0.0) < 1e-3 }
        val fmCcm = if (isIdentity) ccmFromForwardMatrix() else null
        when {
            fmCcm != null -> sb.append("ccm ${fmCcm.joinToString(" ")}\nccm_source forward_matrix\n")
            resCcm != null -> sb.append("ccm ${resCcm.joinToString(" ")}\nccm_source capture_result\n")
        }
        first?.get(CaptureResult.SENSOR_NOISE_PROFILE)?.let { np ->
            if (np.isNotEmpty()) {
                val a = np.map { it.first }.average(); val b = np.map { it.second }.average()
                sb.append("noise_profile $a $b\n")
            }
        }
        for (i in 0 until count) {
            val r = results[timestamps[i]]
            sb.append("frame $i ${timestamps[i]} ${r?.get(CaptureResult.SENSOR_EXPOSURE_TIME) ?: 0} ${r?.get(CaptureResult.SENSOR_SENSITIVITY) ?: 0}\n")
        }
        return sb.toString()
    }

    /** 기준 광원이 D65인 ForwardMatrix(없으면 1번)로 sensor RGB(WB 후) → 선형 sRGB. 행 우선 9개, 없으면 null */
    private fun ccmFromForwardMatrix(): List<Double>? {
        val d65 = CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1_D65
        val fm = when {
            chars.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1) == d65 -> chars.get(CameraCharacteristics.SENSOR_FORWARD_MATRIX1)
            chars.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT2)?.toInt() == d65 -> chars.get(CameraCharacteristics.SENSOR_FORWARD_MATRIX2)
            else -> chars.get(CameraCharacteristics.SENSOR_FORWARD_MATRIX1)
        } ?: return null
        val f = (0 until 3).flatMap { r -> (0 until 3).map { c -> fm.getElement(c, r).toDouble() } }
        val xyz2srgb = doubleArrayOf(3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415, 0.0334540, 0.0719453, -0.2289914, 1.4052427)
        return (0 until 3).flatMap { r -> (0 until 3).map { c -> (0 until 3).sumOf { k -> xyz2srgb[r * 3 + k] * f[k * 3 + c] } } }
    }

    private fun writeDisk() {
        val d = File(ctx.getExternalFilesDir(null), "burst_${System.currentTimeMillis()}").apply { mkdirs() }
        for (i in 0 until count) {
            FileOutputStream(File(d, "frame_%02d.raw16".format(i))).channel.use { it.write(slots[i].duplicate()) }
        }
        File(d, "meta.txt").writeText(metaText)
        dir = d
    }
}
