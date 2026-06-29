package ai.thetahealth.mirobody

class NativeBridge {

    private var handle: Long = 0L

    fun start(
        configPath: String,
        dataDir: String,
        openaiKey: String,
        geminiKey: String,
        listenPort: Int,
    ): Boolean {
        // Pure-client build (libmirobody.so not packaged): no embedded server to run.
        if (!available) return false
        if (handle != 0L) return true
        handle = nativeStart(configPath, dataDir, openaiKey, geminiKey, listenPort)
        return handle != 0L
    }

    fun stop() {
        if (handle == 0L) return
        nativeStop(handle)
        handle = 0L
    }

    fun isRunning(): Boolean = handle != 0L && nativeIsRunning(handle)

    fun listenPort(): Int = if (handle == 0L) -1 else nativeListenPort(handle)

    private external fun nativeStart(
        configPath: String,
        dataDir: String,
        openaiKey: String,
        geminiKey: String,
        listenPort: Int,
    ): Long

    private external fun nativeStop(handle: Long)
    private external fun nativeIsRunning(handle: Long): Boolean
    private external fun nativeListenPort(handle: Long): Int

    companion object {
        // The embedded C++ server (libmirobody.so) is only packaged when the arm64
        // prebuilt deps exist at build time (see app/build.gradle.kts). On a pure-client
        // build the library is absent; loading it must NOT crash the app — MirobodyService
        // checks `available`/start() and stops itself, leaving the UI running as a plain
        // client against the configured BASE_URL.
        val available: Boolean = try {
            System.loadLibrary("mirobody")
            true
        } catch (e: UnsatisfiedLinkError) {
            false
        }
    }
}
