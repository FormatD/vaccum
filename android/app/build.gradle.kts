plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.vacuum.android"
    compileSdk = 34

    defaultConfig {
        applicationId = "dev.vacuum.android"
        // 24 is the floor for the AudioRecord source preset used here
        // (VOICE_RECOGNITION) behaving consistently across handsets.
        minSdk = 24
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // The core is plain C++ with no platform-specific code, so all four
            // ABIs build from the same sources.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
        }
        externalNativeBuild {
            cmake {
                // The core needs C++20; the NDK's clang has supported it since r21.
                cppFlags += "-std=c++20"
                arguments += "-DANDROID_STL=c++_static"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            // No signing config: this is a sample, and shipping a checked-in
            // keystore would be worse than shipping none.
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
        debug {
            // Keep the native side debuggable; a stripped DSP library is very
            // hard to reason about when a decode fails.
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    // For registerForActivityResult.
    implementation("androidx.activity:activity-ktx:1.9.2")
}
