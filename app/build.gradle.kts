plugins {
    alias(libs.plugins.android.application)
}

base {
    archivesName = "pico-qr-reader"
}

android {
    namespace = "dev.pico.qrreader"
    compileSdk = 37
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "dev.pico.qrreader"
        minSdk = 29
        // PICO OS 5 is based on Android 14.
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"

        ndk {
            // PICO 4 / 4 Ultra are arm64 only, and the vendored OpenXR loader
            // is only shipped for arm64-v8a.
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_static"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    // Release signing is configured from the environment so that CI can sign
    // tagged builds; without it the release APK is left unsigned.
    val releaseKeystore = providers.environmentVariable("RELEASE_KEYSTORE_PATH").orNull
    signingConfigs {
        if (releaseKeystore != null) {
            create("release") {
                storeFile = file(releaseKeystore)
                storePassword = providers.environmentVariable("RELEASE_KEYSTORE_PASSWORD").get()
                keyAlias = providers.environmentVariable("RELEASE_KEY_ALIAS").get()
                keyPassword = providers.environmentVariable("RELEASE_KEY_PASSWORD").get()
            }
        }
    }

    buildTypes {
        release {
            // Java methods are called from native code via JNI, so keep R8 off
            // rather than maintaining keep rules for a single Activity.
            isMinifyEnabled = false
            if (releaseKeystore != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        // Distributed via PICO Store / sideloading, not Google Play, and the
        // target SDK tracks PICO OS rather than Play's requirements.
        disable += "ExpiredTargetSdkVersion"
    }
}
