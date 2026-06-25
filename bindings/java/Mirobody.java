import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

/**
 * Java binding for libmirobody, using the Foreign Function & Memory API
 * (JEP 442 "Panama"). On JDK 21 the API is a preview feature, so compile and run
 * with --enable-preview; it is final in JDK 22+ (drop the flag and the
 * allocateUtf8String -> allocateFrom rename). No external dependencies — the
 * binding talks straight to the C functions exported by mirobody.h.
 *
 * The same DLL/.so is what every other FFI host loads; this class is just the
 * Java-flavoured wrapper around it.
 */
public final class Mirobody implements AutoCloseable {

    private static final Linker LINKER = Linker.nativeLinker();

    private final Arena arena = Arena.ofConfined();
    private final MethodHandle hStart;
    private final MethodHandle hStop;
    private final MethodHandle hIsRunning;
    private final MethodHandle hListenPort;

    private MemorySegment handle = MemorySegment.NULL;

    /** Load libmirobody from an explicit path (e.g. ...\\build-shared\\mirobody.dll). */
    public Mirobody(String libraryPath) {
        SymbolLookup lib = SymbolLookup.libraryLookup(libraryPath, arena);

        hStart = LINKER.downcallHandle(
            lib.find("mirobody_start").orElseThrow(),
            FunctionDescriptor.of(ValueLayout.ADDRESS,         // -> mirobody_server_t*
                ValueLayout.ADDRESS,   // config_path
                ValueLayout.ADDRESS)); // data_dir

        hStop = LINKER.downcallHandle(
            lib.find("mirobody_stop").orElseThrow(),
            FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));

        hIsRunning = LINKER.downcallHandle(
            lib.find("mirobody_is_running").orElseThrow(),
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));

        hListenPort = LINKER.downcallHandle(
            lib.find("mirobody_listen_port").orElseThrow(),
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    }

    /** A C string in native memory, or NULL for a null/empty Java string. */
    private MemorySegment cstr(String s) {
        return (s == null || s.isEmpty()) ? MemorySegment.NULL : arena.allocateUtf8String(s);
    }

    /**
     * Start the server. Throws on failure (null handle from native side). The
     * listen port and LLM keys come from config (HTTP_PORT / OPENAI_API_KEY /
     * GOOGLE_API_KEY, which also read their environment variables); read the
     * bound port back via {@link #listenPort()}.
     */
    public void start(String configPath, String dataDir) {
        try {
            handle = (MemorySegment) hStart.invoke(cstr(configPath), cstr(dataDir));
        } catch (Throwable t) {
            throw new RuntimeException("mirobody_start failed", t);
        }
        if (handle.equals(MemorySegment.NULL)) {
            throw new RuntimeException("mirobody_start returned NULL (see native log)");
        }
    }

    public boolean isRunning() {
        if (handle.equals(MemorySegment.NULL)) return false;
        try {
            return (int) hIsRunning.invoke(handle) != 0;
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }

    public int listenPort() {
        if (handle.equals(MemorySegment.NULL)) return -1;
        try {
            return (int) hListenPort.invoke(handle);
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }

    public void stop() {
        if (handle.equals(MemorySegment.NULL)) return;
        try {
            hStop.invoke(handle);
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
        handle = MemorySegment.NULL;
    }

    @Override
    public void close() {
        stop();
        arena.close();
    }
}
