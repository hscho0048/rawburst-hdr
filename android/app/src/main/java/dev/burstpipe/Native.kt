package dev.burstpipe

import java.nio.ByteBuffer

/** android/jni.cc. core/ 파이프라인을 앱 생명주기 동안 1개 유지한다 (아레나·세그 델리게이트 재사용). */
object Native {
    init { System.loadLibrary("burstpipe_jni") }

    /** delegate: 0 cpu, 1 gpu, 2 npu. modelPath null이면 보케 없음. 반환: 세그멘터 상태 문자열 */
    external fun init(w: Int, h: Int, maxFrames: Int, modelPath: String?, delegate: Int, threads: Int, cpus: IntArray): String

    /** frames: direct ByteBuffer RAW16 (w*h*2). outRgba: w*h*4 direct. 반환: timings json, 실패 시 "" */
    external fun process(frames: Array<ByteBuffer>, n: Int, metaTxt: String, outRgba: ByteBuffer): String

    /**
     * 디스크 버스트 디렉터리(meta.txt + frame_NN.raw16)를 처리. mask_emu.bin이 있으면 에뮬 세그멘터로 보케.
     * 기기 없이 JNI·보케 경로를 검증하는 용도 (adb push bursts/emu_portrait_half ...). 반환: timings json
     */
    external fun processDir(dir: String, outRgba: ByteBuffer): String
}
