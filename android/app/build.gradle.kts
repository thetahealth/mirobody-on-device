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
    // A space in the resolved NDK path (e.g. a Windows profile like "C:\Users\A B") makes the
    // NDK toolchain invoke the compiler through an 8.3 short name -- clang++.exe becomes CLANG_~1.EXE,
    // which no longer ends in "++", so clang links in C-driver mode and libc++/libc++abi go
    // unresolved. Let a space-free NDK (e.g. the mirror build-prebuilt.cmd creates) override via env.
    System.getenv("MIROBODY_NDK_PATH")?.takeIf { it.isNotBlank() }?.let { ndkPath = it }

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
            // TRUE, i.e. compressed in the APK and unpacked by the installer, which is
            // the opposite of the modern default and of what the 16 KB page work wanted.
            //
            // llama.cpp is why. Its CPU kernels ship as several libggml-cpu-<arch>.so
            // modules and ggml picks one at startup by scanning a directory and dlopening
            // what it finds. With uncompressed packaging there is no directory to scan:
            // the libraries stay inside the APK, nativeLibraryDir holds nothing, and the
            // scan comes back empty. Extracting them ourselves is not a way out either --
            // W^X blocks dlopen from an app-writable path on API 29+.
            //
            // The 16 KB requirement is unaffected: it is about ELF segment alignment,
            // which android/build-llama.cmd and the root CMakeLists set with
            // -Wl,-z,max-page-size=16384. Uncompressed packaging is one way to satisfy
            // the loader, not the requirement itself. The cost is disk -- the libraries
            // exist twice, in the APK and unpacked -- and that is the price of one APK
            // that runs fast kernels on new phones without SIGILLing on old ones.
            useLegacyPackaging = true
        }
    }
}

// The in-process C++ server (libmirobody.so) is built from the repo-root CMakeLists.txt via the
// NDK. Wiring the externalNativeBuild requires prebuilt dependencies under android/prebuilt/<ABI>/.
// Until those exist we skip the native build entirely so the app still assembles as a pure Kotlin
// client; MirobodyService degrades gracefully when the .so is absent. Drop the prebuilts in place
// and rebuild to enable embedding -- no edit here needed.
//
// Two properties override that default (a third, mirobody.baseUrl, is just below). All are
// settable in gradle.properties, in Android Studio's Settings -> Build -> Compiler ->
// "Command-line Options", or per invocation. In PowerShell quote the whole argument --
// unquoted, `-Pmirobody.native=false` is split and Gradle looks for a task named
// `.native=false`:
//
//   -Pmirobody.abi=x86_64        which ABI to build the embedded server for. Pair it with
//                                `build-prebuilt.cmd x86_64` (the script takes the same names).
//                                An x86_64 emulator on an x86_64 host keeps hardware
//                                acceleration, so this is the only way to exercise the embedded
//                                server at full speed without a physical arm64 device.
//   -Pmirobody.native=false      force a pure-client APK even with the prebuilts in place. Also
//                                the supported way to compile-verify Kotlin without invoking
//                                CMake -- no need to rename android/prebuilt/ out of the way.
val androidAbi = (project.findProperty("mirobody.abi") as String?)
    ?.takeIf { it.isNotBlank() } ?: "arm64-v8a"
val nativeOptIn = (project.findProperty("mirobody.native") as String?)?.toBoolean() ?: true
val nativeServerEnabled = nativeOptIn && rootProject.file("prebuilt/$androidAbi").exists()

// The on-device LLM engine (llama.cpp + GGUF), built into libmirobody.so when the SDK
// android/build-llama.cmd produces is present. Gated on the directory the same way the
// server is gated on prebuilt/<abi>: drop it in and the engine is on, delete it and the
// app still builds -- src/llm/local.cpp compiles a stub whose available() is false.
//
// This is a SECOND on-device runtime, not a replacement: LiteRT-LM (.litertlm, a Gradle
// dependency) stays, and a model's spec says which engine runs it. The two answer very
// different questions -- see docs/on-device-llm.md.
val llamaSdkDir = rootProject.file("prebuilt/llama-sdk/$androidAbi")
val llamaEnabled = nativeServerEnabled && llamaSdkDir.resolve("include/llama.h").exists()

// The SDK is shared objects, so they have to be packaged as well as linked against --
// libmirobody.so carries a DT_NEEDED on libllama.so, and the libggml-cpu-* modules are
// dlopened by name at startup and referenced by nothing at link time at all.
//
// A staging copy rather than a srcDir straight onto the SDK, because AGP wants
// <root>/<abi>/*.so and the SDK is <abi>/lib/*.so. Building the layout AGP expects is a
// two-line Copy; reshaping the SDK to suit one consumer would make it worse for the
// others (harmony/, qt/) that just want include/ and lib/.
// A plain File, resolved now: the Android source-set API rejects a lazy Provider
// ("You cannot add Provider instances to the Android SourceSet API"), so the directory
// has to be nameable at configuration time. The Copy task below still fills it lazily.
val llamaJniStage = layout.buildDirectory.dir("llama-jni").get().asFile
// Everything the task body touches is resolved to a plain File or String out here. A
// lambda that reads a script-level val (`onlyIf { llamaEnabled }`, a nested `from { }`)
// captures the build script itself, which the configuration cache refuses to serialize.
val llamaLibDir = llamaSdkDir.resolve("lib")
val llamaJniAbiDir = llamaJniStage.resolve(androidAbi)
val stageLlamaJniLibs = tasks.register<Copy>("stageLlamaJniLibs") {
    // No guard on llamaEnabled: in a client build llamaLibDir simply does not exist and
    // Copy stages nothing, which is the same outcome with one less thing to keep in sync.
    from(llamaLibDir)
    include("*.so")
    into(llamaJniAbiDir)
}
android {
    sourceSets["main"].jniLibs.srcDir(llamaJniStage)
}
tasks.named("preBuild") { dependsOn(stageLlamaJniLibs) }

// Where the app points when the user has not chosen a backend: the public test server,
// embedded build or not.
//
// An embedded build does serve on 127.0.0.1:8080 in-process, and defaulting there was the
// tidy answer — it is the server this very APK is carrying. But it starts EMPTY. A fresh
// install has no account on it and no way to make one, so the first thing a new user meets
// is a sign-in screen nothing can get them past. Reaching the in-process server is a
// two-tap change in Settings → Backend; an account that does not exist is not.
//
// -Pmirobody.baseUrl=... still overrides (e.g. http://10.0.2.2:18080 for a server running
// on the emulator's host).
val defaultBaseUrl = (project.findProperty("mirobody.baseUrl") as String?)
    ?.takeIf { it.isNotBlank() }
    ?: "https://test.mirobody.ai"

// No companion "is this an embedded build" flag: MainActivity already gates on
// NativeBridge.available, which reports whether the .so actually loaded rather than whether
// the build intended it to — the more accurate of the two, and one source of truth.
android {
    defaultConfig {
        buildConfigField("String", "DEFAULT_BASE_URL", "\"$defaultBaseUrl\"")
    }
}
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
                        //
                        // The path is interpolated with the single fixed `androidAbi` (abiFilters
                        // restricts the build to it) rather than CMake's `${ANDROID_ABI}` macro:
                        // AGP 9.0 no longer expands that macro in cmake arguments and errors with
                        // "Unrecognized macro ANDROID_ABI". Hardcoding the sole ABI is equivalent.
                        "-DCMAKE_FIND_ROOT_PATH=${rootProject.projectDir}/prebuilt/$androidAbi",
                        "-DCMAKE_PREFIX_PATH=${rootProject.projectDir}/prebuilt/$androidAbi",
                    )
                    if (llamaEnabled) {
                        // Absolute, and NOT via CMAKE_PREFIX_PATH: the NDK toolchain sets
                        // CMAKE_FIND_ROOT_PATH_MODE_*=ONLY, and local.cpp's find_library
                        // for the llama/ggml archives passes NO_CMAKE_FIND_ROOT_PATH to
                        // escape that -- so it wants a real path, not a prefix to search.
                        arguments += listOf(
                            "-DMIROBODY_ONDEVICE_LLM=ON",
                            "-DLLAMA_CPP_DIR=${llamaSdkDir.invariantSeparatorsPath}",
                        )
                    }
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

    // On-device private LLM: LiteRT-LM runs Gemma 4 entirely on-device (no network,
    // no server). The ~2.5 GB .litertlm model is downloaded at runtime by ModelManager.
    implementation(libs.litertlm.android)

    // On-device GenAI utilities via Gemini Nano (AICore) — used by MlKitTextService for
    // draft rewriting/summarization where the device supports it. Degrades gracefully.
    implementation(libs.mlkit.genai.rewriting)
    implementation(libs.mlkit.genai.summarization)
    // ML Kit GenAI returns Guava ListenableFuture; this adds the coroutine await() for it.
    implementation(libs.kotlinx.coroutines.guava)

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
