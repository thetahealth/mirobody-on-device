package ai.thetahealth.mirobody;

/**
 * JNI binding for the mirobody server — the in-process path for desktop JVMs.
 * Backed by mirobody_jni.{dll,so,dylib} (src/platform/jni_bridge.cpp), the same
 * Java class the Android app binds. Unlike the Panama FFM binding in
 * bindings/java/Mirobody.java, JNI native calls run cleanly under HotSpot.
 *
 * Load the native library before use; it must be on java.library.path:
 *   -Djava.library.path=...\\build-shared
 * and its transitive deps (libcurl, openssl, …) reachable on PATH / LD_LIBRARY_PATH.
 *
 * The returned long is an opaque native handle; pass it back to the other calls,
 * and always nativeStop() it to release the server.
 */
public final class NativeBridge implements AutoCloseable {

    static {
        System.loadLibrary("mirobody_jni");
    }

    private long handle = 0;

    private native long nativeStart(String configPath, String dataDir,
                                    String openaiKey, String geminiKey, int listenPort);
    private native void nativeStop(long handle);
    private native boolean nativeIsRunning(long handle);
    private native int nativeListenPort(long handle);

    /** Start the server. Throws if the native side returns a null handle. */
    public void start(String configPath, String dataDir,
                      String openaiKey, String geminiKey, int listenPort) {
        handle = nativeStart(configPath, dataDir, openaiKey, geminiKey, listenPort);
        if (handle == 0) {
            throw new RuntimeException("mirobody nativeStart failed (see native log)");
        }
    }

    public boolean isRunning() {
        return handle != 0 && nativeIsRunning(handle);
    }

    public int listenPort() {
        return handle == 0 ? -1 : nativeListenPort(handle);
    }

    public void stop() {
        if (handle != 0) {
            nativeStop(handle);
            handle = 0;
        }
    }

    @Override
    public void close() {
        stop();
    }
}
