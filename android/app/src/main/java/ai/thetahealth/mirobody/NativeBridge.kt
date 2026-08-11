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

    // ---- On-device LLM (llama.cpp + GGUF) ------------------------------------------
    // The second on-device runtime, beside LiteRT-LM. Both return JSON rather than a
    // typed object: this crosses a JNI boundary that exists for the server already, and
    // a string keeps the bridge free of per-field marshalling for what is, on this side,
    // a diagnostic and a benchmark.

    /**
     * Point llama.cpp at [dir] for its dlopen-able CPU backends -- one
     * `libggml-cpu-<arch>.so` per instruction-set level, of which it loads the best this
     * chip can run. Call once at startup, before anything else touches the engine.
     *
     * [dir] must be `applicationInfo.nativeLibraryDir`: ggml searches beside the
     * executable by default, which on Android is `/system/bin/app_process`. It also has
     * to contain real files, so the APK cannot compress its libraries
     * (`useLegacyPackaging = true` in build.gradle.kts).
     */
    fun localBackendPath(dir: String) {
        if (available) nativeLocalBackendPath(dir)
    }

    /**
     * Engine availability plus the CPU-feature picture, as JSON.
     *
     * `has` is the silicon. `variant` is which CPU module got loaded -- the answer to
     * "are we actually running the fast kernels on this chip", which in a dispatched
     * build replaces the old compile-time `built` view.
     */
    fun localStatus(): String =
        if (!available) """{"available":false}""" else nativeLocalStatus()

    /**
     * Run one timed turn on [modelPath] and return `llm::LocalStats` as JSON.
     *
     * BLOCKS for the whole turn and loads a few GB, so call it off the main thread.
     * Deliberately builds a fresh engine each time: the load is half of what is being
     * measured, and a reused one would hide it.
     */
    fun localBenchmark(modelPath: String, prompt: String, decodeTokens: Int, threads: Int): String =
        if (!available) """{"ok":false,"error":"native library not built in"}"""
        else nativeLocalBenchmark(modelPath, prompt, decodeTokens, threads)

    // ---- Streaming chat over llama.cpp ---------------------------------------------
    // Handle-based: the handle owns the weights, so turn two does not re-read a few GB.
    // Everything here except cancel() BLOCKS; the engine keeps it off the main thread.

    /** One event from a running turn. Return false to stop the turn at the next token. */
    fun interface LocalEventSink {
        fun onEvent(type: Int, text: String): Boolean
    }

    /** Open an engine over [modelPath]. Returns 0 on failure. Cheap — nothing is read yet. */
    fun localOpen(modelPath: String, threads: Int, thinking: Int): Long =
        if (!available) 0L else nativeLocalOpen(modelPath, threads, thinking)

    fun localClose(handle: Long) {
        if (available && handle != 0L) nativeLocalClose(handle)
    }

    /** Ask the in-flight turn to stop. Safe from another thread; a no-op when idle. */
    fun localCancel(handle: Long) {
        if (available && handle != 0L) nativeLocalCancel(handle)
    }

    /** Read the weights now. "" on success, otherwise the reason. BLOCKS for seconds. */
    fun localLoad(handle: Long): String =
        if (!available || handle == 0L) "native library not built in" else nativeLocalLoad(handle)

    fun localLoaded(handle: Long): Boolean =
        available && handle != 0L && nativeLocalLoaded(handle)

    /**
     * Run one turn, streaming into [sink]. BLOCKS until the reply ends or is cancelled.
     *
     * [roles] and [contents] are parallel: `roles[i]` is "user" or "assistant" for
     * `contents[i]`. `<think>` is split natively, so Thinking arrives as its own event
     * type and the caller never sees a tag.
     */
    fun localGenerate(
        handle: Long,
        roles: Array<String>,
        contents: Array<String>,
        systemPrompt: String,
        sink: LocalEventSink,
    ): Boolean =
        available && handle != 0L && nativeLocalGenerate(handle, roles, contents, systemPrompt, sink)

    private external fun nativeLocalBackendPath(dir: String)
    private external fun nativeLocalStatus(): String
    private external fun nativeLocalBenchmark(
        modelPath: String,
        prompt: String,
        decodeTokens: Int,
        threads: Int,
    ): String

    private external fun nativeLocalOpen(modelPath: String, threads: Int, thinking: Int): Long
    private external fun nativeLocalClose(handle: Long)
    private external fun nativeLocalCancel(handle: Long)
    private external fun nativeLocalLoad(handle: Long): String
    private external fun nativeLocalLoaded(handle: Long): Boolean
    private external fun nativeLocalGenerate(
        handle: Long,
        roles: Array<String>,
        contents: Array<String>,
        systemPrompt: String,
        sink: LocalEventSink,
    ): Boolean

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
