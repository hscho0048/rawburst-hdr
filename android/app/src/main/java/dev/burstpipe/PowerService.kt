package dev.burstpipe

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.os.BatteryManager
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import java.io.File

/**
 * 배터리 전력 샘플러 (기기 전체 기준). 삼성·샤오미는 shell에서 power_supply sysfs가 막혀 있어
 * cli 실험도 이 서비스가 샘플을 남기고 cli는 구간 경계(CLOCK_BOOTTIME ns)만 기록한다 → PC에서 적분 (tools/power_analyze.py).
 *
 *  시작: adb shell am start-foreground-service -n dev.burstpipe/.PowerService --es name RUN [--ei period_ms 50]
 *  종료: adb shell am start-foreground-service -n dev.burstpipe/.PowerService --es cmd stop   (CSV flush 후 종료)
 *  출력: /sdcard/Android/data/dev.burstpipe/files/power/RUN.csv
 *        t_ns,current_raw,charge_uah,voltage_mv,temp_dc,plugged,status
 *
 * - current_raw: BATTERY_PROPERTY_CURRENT_NOW 원값. 단위(µA/mA)·부호(방전 음/양)는 기기마다 다르다 → 검증 결과는 docs/power.md
 * - voltage_mv: ACTION_BATTERY_CHANGED의 EXTRA_VOLTAGE (방송 때만 갱신 → 수 분 단위로 느림. 부하 시 전압 강하는 반영 못 함)
 * - PARTIAL_WAKE_LOCK: 화면 꺼진 cli 실험에서 기기가 suspend로 가지 않게 (idle 기준선도 같은 조건)
 */
class PowerService : Service() {
    private var th: Thread? = null
    @Volatile private var running = false
    private var wl: PowerManager.WakeLock? = null
    private var file: File? = null
    private val buf = StringBuilder(1 shl 20)
    @Volatile private var voltageMv = 0
    @Volatile private var tempDc = 0
    @Volatile private var plugged = 0
    @Volatile private var status = 0

    private val rx = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) = update(i)
    }

    private fun update(i: Intent?) {
        i ?: return
        voltageMv = i.getIntExtra(BatteryManager.EXTRA_VOLTAGE, voltageMv)
        tempDc = i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, tempDc)
        plugged = i.getIntExtra(BatteryManager.EXTRA_PLUGGED, plugged)
        status = i.getIntExtra(BatteryManager.EXTRA_STATUS, status)
    }

    override fun onBind(i: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startFg()
        if (intent?.getStringExtra("cmd") == "stop") { stopSampling(); stopSelf(); return START_NOT_STICKY }
        if (running) { Log.w(TAG, "already sampling → ${file?.name}"); return START_NOT_STICKY }
        val name = intent?.getStringExtra("name") ?: "power_${System.currentTimeMillis()}"
        val period = intent?.getIntExtra("period_ms", 50) ?: 50
        startSampling(name, period)
        return START_NOT_STICKY
    }

    private fun startFg() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel("power", "power sampler", NotificationManager.IMPORTANCE_LOW))
        val n = Notification.Builder(this, "power").setContentTitle("burstpipe power sampler")
            .setSmallIcon(android.R.drawable.ic_lock_idle_charging).build()
        startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
    }

    private fun startSampling(name: String, periodMs: Int) {
        val dir = File(getExternalFilesDir(null), "power").apply { mkdirs() }
        file = File(dir, "$name.csv")
        buf.setLength(0)
        buf.append("t_ns,current_raw,charge_uah,voltage_mv,temp_dc,plugged,status\n")
        update(registerReceiver(rx, IntentFilter(Intent.ACTION_BATTERY_CHANGED)))
        wl = getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "burstpipe:power").apply { acquire() }
        val bm = getSystemService(BatteryManager::class.java)
        val energy = bm.getLongProperty(BatteryManager.BATTERY_PROPERTY_ENERGY_COUNTER)
        Log.i(TAG, "power sampling start $name period=${periodMs}ms energy_counter=$energy")
        running = true
        th = Thread({
            var next = SystemClock.elapsedRealtimeNanos()
            var lastFlush = next
            while (running) {
                val t = SystemClock.elapsedRealtimeNanos()
                val cur = bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
                val cc = bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER)
                synchronized(buf) {
                    buf.append(t).append(',').append(cur).append(',').append(cc).append(',').append(voltageMv).append(',')
                        .append(tempDc).append(',').append(plugged).append(',').append(status).append('\n')
                    // 5초마다 파일에 붙인다: 앱이 force-stop/동결돼도 그 전 샘플은 남는다 (쓰기 비용 ~수 ms, idle·작업 구간 공통)
                    if (t - lastFlush > 5_000_000_000L || buf.length > (1 shl 20) - 256) { flush(); lastFlush = t }
                }
                next += periodMs * 1_000_000L
                val sleep = (next - SystemClock.elapsedRealtimeNanos()) / 1_000_000L
                if (sleep > 0) Thread.sleep(sleep) else next = SystemClock.elapsedRealtimeNanos()
            }
        }, "power-sampler").apply { priority = Thread.MAX_PRIORITY; start() }
    }

    private fun flush() {
        val f = file ?: return
        f.appendText(buf.toString())
        buf.setLength(0)
    }

    private fun stopSampling() {
        if (!running) return
        running = false
        th?.join(1000)
        synchronized(buf) { flush() }
        runCatching { unregisterReceiver(rx) }
        wl?.let { if (it.isHeld) it.release() }
        File(file!!.path + ".done").writeText("ok\n")
        Log.i(TAG, "power sampling stop → ${file?.absolutePath}")
    }

    override fun onDestroy() { stopSampling(); super.onDestroy() }

    companion object { const val TAG = "burstpipe" }
}
