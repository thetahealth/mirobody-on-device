plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "ai.thetahealth.mirobody"
    // 36 (Android 16): required by androidx.health.connect:connect-client 1.1.0-rc03.
    // compileSdk only governs which APIs compile; targetSdk/minSdk stay as-is.
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "ai.thetahealth.mirobody"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }

        // WeChat OpenSDK app id (Open Platform "Mobile Application"). Read by
        // WechatAuthRepository / WXEntryActivity to register with the SDK. Set the
        // real value here or via -PwechatAppId=... ; blank disables WeChat sign-in.
        buildConfigField(
            "String",
            "WECHAT_APP_ID",
            "\"${(project.findProperty("wechatAppId") as String?) ?: ""}\"",
        )
    }

    // Two device targets share one codebase. `phone` is the default (unchanged behaviour);
    // `watch` is for small AOSP wearables (~410x502, Android 9). Both keep the same
    // applicationId/auth config; only BuildConfig.IS_WATCH and the optional src/watch/res
    // overlay differ. Layout adapts at runtime via LayoutInfo regardless of flavour.
    flavorDimensions += "device"
    productFlavors {
        create("phone") {
            dimension = "device"
            isDefault = true
            buildConfigField("boolean", "IS_WATCH", "false")
        }
        create("watch") {
            dimension = "device"
            versionNameSuffix = "-watch"
            buildConfigField("boolean", "IS_WATCH", "true")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

// The in-process C++ server (libmirobody.so) is built from the repo-root CMakeLists.txt via the
// NDK. Wiring the externalNativeBuild requires arm64 prebuilt dependencies under android/prebuilt/.
// Until those exist we skip the native build entirely so the app still assembles as a pure Kotlin
// client; MirobodyService degrades gracefully when the .so is absent. Drop the prebuilts in place
// and rebuild to enable embedding -- no edit here needed.
val androidAbi = "arm64-v8a"
val nativeServerEnabled = rootProject.file("prebuilt/$androidAbi").exists()
if (nativeServerEnabled) {
    android {
        defaultConfig {
            ndk {
                abiFilters += androidAbi
            }
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DANDROID_STL=c++_shared",
                        // CMAKE_FIND_ROOT_PATH (not just CMAKE_PREFIX_PATH): the NDK toolchain
                        // sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE/LIBRARY/INCLUDE=ONLY, so
                        // find_package/find_library only search the find-root paths. Seeding the
                        // prebuilt prefix here makes the deps under prebuilt/<abi>/{lib,include,share}
                        // resolvable; the toolchain appends the NDK sysroot after it.
                        "-DCMAKE_FIND_ROOT_PATH=${rootProject.projectDir}/prebuilt/\${ANDROID_ABI}",
                        "-DCMAKE_PREFIX_PATH=${rootProject.projectDir}/prebuilt/\${ANDROID_ABI}",
                    )
                    cppFlags += "-std=c++17"
                }
            }
        }
        externalNativeBuild {
            cmake {
                path = file("../../CMakeLists.txt")
                version = "3.22.1"
            }
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation(libs.androidx.material.icons.extended)
    implementation(libs.androidx.navigation.compose)

    implementation(libs.androidx.datastore.preferences)

    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.coroutines.play.services)

    implementation(platform(libs.firebase.bom))
    implementation(libs.firebase.auth.ktx)

    // WeChat OpenSDK (Sign in with WeChat). "without-mta" drops Tencent's
    // analytics module. Used by WechatAuthRepository / wxapi.WXEntryActivity.
    implementation(libs.wechat.opensdk)

    implementation(libs.okhttp)
    implementation(libs.okhttp.sse)
    implementation(libs.okhttp.logging.interceptor)
    implementation(libs.retrofit)
    implementation(libs.retrofit.kotlinx.serialization.converter)

    // On-device health ingestion (data/health). Health Connect covers GMS devices;
    // HMS Health Kit covers Huawei. The HMS path additionally needs the AppGallery
    // Connect agconnect plugin + agconnect-services.json and an approved Health Kit
    // entitlement before it functions (HmsHealthSource is inert until then).
    implementation(libs.androidx.health.connect)
    implementation(libs.hms.health)

    implementation(libs.coil.compose)
    implementation(libs.coil.svg)

    implementation(libs.markwon.core)
    implementation(libs.markwon.inline.parser)
    implementation(libs.markwon.ext.latex)
    implementation(libs.markwon.ext.tables)
    implementation(libs.markwon.ext.strikethrough)
    implementation(libs.markwon.html)
    implementation(libs.markwon.linkify)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.ui.test.junit4)
    debugImplementation(libs.androidx.ui.tooling)
    debugImplementation(libs.androidx.ui.test.manifest)
}
