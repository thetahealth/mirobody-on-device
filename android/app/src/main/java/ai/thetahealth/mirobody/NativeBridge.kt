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
        init {
            System.loadLibrary("mirobody")
        }
    }
}
