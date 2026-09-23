import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.burstpipe"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "dev.burstpipe"
        minSdk = 30
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"
        // arm64-v8a: Galaxy C55. x86_64: Android 에뮬레이터 (카메라 RAW는 에뮬레이터 HAL로 검증)
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild { cmake { arguments += listOf("-DANDROID_PLATFORM=android-30") } }
    }
    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" }
    }
    // LiteRT .so (scripts/fetch_litert.sh)와 세그 모델은 있으면 자동 포함
    sourceSets["main"].jniLibs.srcDirs("../../third_party/litert/lib", "../../third_party/qnn/lib")
    // QNN HTP: DSP 쪽 skel 로더가 파일 경로를 요구 → .so를 APK에서 풀어 nativeLibraryDir에 둔다
    packaging { jniLibs { useLegacyPackaging = true } }
    sourceSets["main"].assets.srcDirs("../../models")
    buildTypes {
        release { isMinifyEnabled = false; signingConfig = signingConfigs.getByName("debug") }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin { compilerOptions { jvmTarget.set(JvmTarget.JVM_17) } }
