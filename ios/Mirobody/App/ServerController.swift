import Foundation

/// Starts and stops the in-process C++ server, the iOS analogue of Android's
/// `MirobodyService`. Where Android loads `libmirobody.so` over JNI and runs it in
/// a foreground service, iOS links `mirobody.xcframework` (the C API in
/// `src/mirobody.h`) and calls `mirobody_start` / `mirobody_stop` directly.
///
/// The whole thing is gated behind `MIROBODY_EMBEDDED`. In a pure-client build the
/// flag is undefined, no native symbols are referenced, and the app talks to a
/// remote backend over the configured base URL — exactly like the Android client
/// degrades gracefully when the `.so` is absent.
final class ServerController: ObservableObject {

    /// True once the embedded server is running on loopback. Always false in a
    /// pure-client build.
    @Published private(set) var isRunning = false

    /// Port the embedded server is bound to (matches `SettingsStore.defaultBaseURL`).
    let listenPort: Int32 = 8080

#if MIROBODY_EMBEDDED
    private var handle: OpaquePointer?

    /// Launch the embedded server so the client can reach it at
    /// `http://localhost:8080`. The listen port and the OpenAI/Gemini keys come
    /// from the bundled config.yml (see `configPath`) — set HTTP_PORT there to
    /// move off 8080, and the LLM keys to enable upstream chat.
    func start() {
        guard handle == nil else { return }
        let started = configPath.withCStringOrNil { cfg in
            dataDir.withCStringOrNil { dir in
                mirobody_start(cfg, dir)
            }
        }
        guard let started else {
            NSLog("[Mirobody] embedded server failed to start")
            return
        }
        handle = started
        isRunning = mirobody_is_running(started) != 0
        NSLog("[Mirobody] embedded server listening on 127.0.0.1:\(mirobody_listen_port(started))")
    }

    func stop() {
        guard let handle else { return }
        mirobody_stop(handle)
        self.handle = nil
        isRunning = false
    }

    /// Optional bundled config. The bridge falls back to compiled-in defaults when
    /// this is nil, which is enough for loopback; bundle a `config.yml` in the app
    /// target to override providers, keys, etc.
    private var configPath: String? {
        Bundle.main.path(forResource: "config", ofType: "yaml")
    }

    /// Writable directory handed to the server for any on-device state.
    private var dataDir: String? {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?.path
    }
#else
    /// Pure-client build: nothing to start. The app uses the configured base URL.
    func start() {}
    func stop() {}
#endif
}

private extension Optional where Wrapped == String {
    /// Bridges an optional Swift string to a `const char *` for the C API, passing
    /// NULL when nil so the bridge applies its own fallback.
    func withCStringOrNil<R>(_ body: (UnsafePointer<CChar>?) -> R) -> R {
        switch self {
        case .some(let s): return s.withCString(body)
        case .none: return body(nil)
        }
    }
}
