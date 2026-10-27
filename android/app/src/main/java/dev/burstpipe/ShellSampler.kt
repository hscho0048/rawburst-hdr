package dev.burstpipe

import android.os.IBinder
import android.os.SystemClock
import java.io.File

/**
 * shell 권한 배터리 샘플러 (앱 프로세스가 아님). MIUI PowerKeeper의 AutoPowerKill은 화면이 꺼지고 7–16분 뒤
 * 포그라운드 서비스까지 force-stop 한다 (exit-info: "stop dev.burstpipe due to AutoPowerKill") → PowerService 샘플이 끊긴다.
 * shell uid 프로세스는 그 대상이 아니므로 같은 값을 shell에서 읽는다:
 *   CLASSPATH=$(pm path dev.burstpipe | cut -d: -f2) app_process /system/bin dev.burstpipe.ShellSampler OUT.csv [period_ms]
 * - 전류·쿨롱: BatteryManager.getIntProperty와 같은 경로 (IBatteryPropertiesRegistrar.getProperty, CURRENT_NOW=2, CHARGE_COUNTER=1)
 * - 전압: shell이 읽을 수 있는 thermal_zone "vbat"(mV, 실시간) × 셀 배율. 배율 = dumpsys battery voltage / vbat (60초마다 갱신,
 *   Mix 4는 2셀 직렬이라 ≈2, C55 ≈1). PowerService(방송 때만 갱신)보다 부하 중 전압 강하를 잘 반영한다.
 * - 출력 형식은 PowerService CSV와 같다 (plugged·status는 dumpsys 기준 60초마다). 5초마다 파일에 붙이고 OUT.stop 파일이 생기면 끝낸다.
 */
object ShellSampler {
    @JvmStatic
    fun main(args: Array<String>) {
        val out = File(args[0])
        val period = args.getOrNull(1)?.toLong() ?: 50L
        val stop = File(out.path + ".stop")
        val reg = registrar()
        val propCls = Class.forName("android.os.BatteryProperty")
        val getProp = reg.javaClass.getMethod("getProperty", Int::class.javaPrimitiveType, propCls)
        val getLong = propCls.getMethod("getLong")
        fun prop(id: Int): Long {
            val p = propCls.getDeclaredConstructor().newInstance()
            getProp.invoke(reg, id, p)
            return getLong.invoke(p) as Long
        }
        val vbat = File("/sys/class/thermal").listFiles()?.firstOrNull { z ->
            runCatching { File(z, "type").readText().trim() == "vbat" }.getOrDefault(false)
        }?.let { File(it, "temp") }
        var scale = 1.0; var plugged = 0; var status = 0; var tempDc = 0; var lastDump = 0L
        fun dump() {
            val txt = runCatching { Runtime.getRuntime().exec(arrayOf("dumpsys", "battery")).inputStream.bufferedReader().readText() }.getOrDefault("")
            fun v(k: String) = Regex("(?m)^\\s*$k: (-?\\d+)").find(txt)?.groupValues?.get(1)?.toIntOrNull()
            val vmv = v("voltage") ?: 0
            val vb = vbat?.let { runCatching { it.readText().trim().toInt() }.getOrNull() } ?: 0
            if (vmv > 0 && vb > 0) scale = Math.round(vmv.toDouble() / vb).coerceAtLeast(1).toDouble()  // 셀 수 (정수)
            plugged = (if (txt.contains("AC powered: true")) 1 else 0) or (if (txt.contains("USB powered: true")) 2 else 0) or
                (if (txt.contains("Wireless powered: true")) 4 else 0)
            status = v("status") ?: 0; tempDc = v("temperature") ?: 0
        }
        dump()
        val sb = StringBuilder(1 shl 16)
        out.writeText("t_ns,current_raw,charge_uah,voltage_mv,temp_dc,plugged,status\n")
        System.err.println("ShellSampler → $out period=${period}ms vbat=${vbat?.path} cells=$scale")
        var next = SystemClock.elapsedRealtimeNanos(); var lastFlush = next
        while (!stop.exists()) {
            val t = SystemClock.elapsedRealtimeNanos()
            if (t - lastDump > 60_000_000_000L) { dump(); lastDump = t }
            val cur = prop(2); val cc = prop(1)
            val vb = vbat?.let { runCatching { it.readText().trim().toInt() }.getOrNull() } ?: 0
            sb.append(t).append(',').append(cur).append(',').append(cc).append(',').append((vb * scale).toLong()).append(',')
                .append(tempDc).append(',').append(plugged).append(',').append(status).append('\n')
            if (t - lastFlush > 5_000_000_000L) { out.appendText(sb.toString()); sb.setLength(0); lastFlush = t }
            next += period * 1_000_000L
            val s = (next - SystemClock.elapsedRealtimeNanos()) / 1_000_000L
            if (s > 0) Thread.sleep(s) else next = SystemClock.elapsedRealtimeNanos()
        }
        out.appendText(sb.toString())
        File(out.path + ".done").writeText("ok\n")
    }

    private fun registrar(): Any {
        val b = Class.forName("android.os.ServiceManager").getMethod("getService", String::class.java)
            .invoke(null, "batteryproperties") as IBinder
        return Class.forName("android.os.IBatteryPropertiesRegistrar\$Stub").getMethod("asInterface", IBinder::class.java).invoke(null, b)!!
    }
}
