import Foundation

/// Which engine runs this file. The format follows from it: LiteRT-LM takes
/// `.litertlm`, llama.cpp takes `.gguf`, and neither reads the other's.
///
/// Two runtimes on purpose. LiteRT-LM is a Swift package; llama.cpp is compiled into
/// `mirobody.xcframework` (see `ios/build-llama.sh`) and reads any GGUF ever published.
/// They answer different questions — see `docs/on-device-llm.md` — and a model says
/// which one it needs rather than the app guessing from the extension.
enum OnDeviceRuntime {
    case liteRtLm
    case llamaCpp
}

/// One on-device model the app can run — a `.litertlm` for LiteRT-LM or a `.gguf` for
/// llama.cpp, per `runtime`. Each is downloaded at runtime (never bundled); several can
/// coexist and the user picks which to run. `providerCode` is the synthetic
/// chat-provider id it surfaces as in the picker. Mirrors Android's
/// `data/llm/OnDeviceModel.kt`.
struct OnDeviceModelSpec: Identifiable, Hashable {
    let id: String
    /// Non-localized display name, e.g. "Gemma 4 E2B" (kept stable across UI languages).
    let displayName: String
    /// Local filename under Application Support `models/`.
    let fileName: String
    /// Direct Hugging Face download URL (`resolve/main/<file>?download=true`).
    let downloadURL: URL
    /// Approximate download size, for the UI to show before Content-Length lands.
    let approxBytes: Int64
    /// Recommended device RAM (peak inference footprint is device/context-dependent).
    let recommendedRam: String
    /// Absolute path to a user-imported model file that lives OUTSIDE the app sandbox
    /// (picked from the Files app), or nil for a catalog model under Application Support.
    /// When set, the model is loaded from here as-is — never downloaded, and never
    /// deleted from disk on removal (it's the user's own file).
    var localPath: String? = nil
    /// Which engine loads this file; the default is the one the catalog started with.
    var runtime: OnDeviceRuntime = .liteRtLm

    var providerCode: String { OnDeviceModel.providerPrefix + id }

    /// True for a user-imported model (referenced by `localPath`), false for a catalog model.
    var isImported: Bool { localPath != nil }
}

/// The catalog of on-device models the app ships support for. Desktop (llama.cpp) is
/// model-agnostic over any GGUF; mobile (LiteRT-LM) ships a curated set of `.litertlm`
/// models, since the runtime only accepts that format.
enum OnDeviceModel {
    /// Prefix marking a synthetic on-device provider code (never collides with a server model).
    static let providerPrefix = "__ondevice__/"

    // Ordered smallest → largest so low-memory devices see the light options first.
    // `approxBytes` is only the pre-flight estimate; the real size comes from the
    // download's Content-Length. Bigger models want more RAM at inference, so a 4B
    // model is best on an 8 GB+ phone.
    static let catalog: [OnDeviceModelSpec] = [
        // Two lanes on purpose, and the Qwen entries are why. `litert-community`
        // publishes nothing above Qwen3.5-0.8B, so the current Qwen generation reaches
        // a phone only as GGUF — which is exactly what the second runtime is for. Gemma
        // stays on `.litertlm`, where Google's own runtime reads the E-series'
        // MatFormer layout properly and beats llama.cpp by 1.5x. Availability, not
        // preference; swap a Qwen entry over the day a `.litertlm` lands and measures
        // faster.
        OnDeviceModelSpec(
            id: "qwen35-2b",
            displayName: "Qwen3.5 2B",
            fileName: "Qwen3.5-2B-Q4_K_M.gguf",
            downloadURL: URL(string:
                "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf?download=true")!,
            approxBytes: 1_280_835_840,
            recommendedRam: "4 GB+",
            runtime: .llamaCpp
        ),
        OnDeviceModelSpec(
            id: "gemma-4-e2b",
            displayName: "Gemma 4 E2B",
            fileName: "gemma-4-E2B-it.litertlm",
            downloadURL: URL(string:
                "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/gemma-4-E2B-it.litertlm?download=true")!,
            approxBytes: 2_588_147_712,
            recommendedRam: "6 GB+"
        ),
        OnDeviceModelSpec(
            id: "qwen35-4b",
            displayName: "Qwen3.5 4B",
            fileName: "Qwen3.5-4B-Q4_K_M.gguf",
            downloadURL: URL(string:
                "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/main/Qwen3.5-4B-Q4_K_M.gguf?download=true")!,
            approxBytes: 2_740_937_888,
            recommendedRam: "8 GB+",
            runtime: .llamaCpp
        ),
        OnDeviceModelSpec(
            id: "gemma-4-e4b",
            displayName: "Gemma 4 E4B",
            fileName: "gemma-4-E4B-it.litertlm",
            downloadURL: URL(string:
                "https://huggingface.co/litert-community/gemma-4-E4B-it-litert-lm/resolve/main/gemma-4-E4B-it.litertlm?download=true")!,
            approxBytes: 3_659_530_240,
            recommendedRam: "8 GB+"
        ),
    ]

    /// Id prefix for a user-imported model, so it never collides with a catalog id.
    static let importedIdPrefix = "imported-"

    /// User-imported models, registered at runtime by `ModelManager` from its persisted
    /// list. Kept alongside `catalog` so `byId` / `byProviderCode` — and therefore
    /// `ProviderInfo.modelSpec` and the whole chat/engine path — resolve imports with no
    /// further changes. Keyed by `OnDeviceModelSpec.id`.
    private static var imported: [String: OnDeviceModelSpec] = [:]

    /// Register (or replace) an imported model so it resolves through `byId`/`byProviderCode`.
    static func registerImported(_ spec: OnDeviceModelSpec) { imported[spec.id] = spec }

    /// Forget an imported model (does not touch the file on disk).
    static func unregisterImported(_ id: String) { imported[id] = nil }

    /// Currently-registered imported models, name-sorted for stable UI ordering.
    static func importedSpecs() -> [OnDeviceModelSpec] {
        imported.values.sorted { $0.displayName < $1.displayName }
    }

    /// Build an imported spec from a picked file's absolute `path` and known `sizeBytes`.
    static func importedSpec(id: String, displayName: String, path: String, sizeBytes: Int64) -> OnDeviceModelSpec {
        OnDeviceModelSpec(
            id: id,
            displayName: displayName,
            fileName: (path as NSString).lastPathComponent,
            downloadURL: URL(string: "https://invalid.local/imported")!,
            approxBytes: sizeBytes,
            recommendedRam: "—",
            localPath: path,
            // The one place the extension IS the answer: an imported file arrives with
            // no spec to ask, so the format has to speak for itself.
            runtime: path.lowercased().hasSuffix(".gguf") ? .llamaCpp : .liteRtLm
        )
    }

    static func byId(_ id: String) -> OnDeviceModelSpec? {
        catalog.first { $0.id == id } ?? imported[id]
    }
    static func byProviderCode(_ code: String) -> OnDeviceModelSpec? {
        catalog.first { $0.providerCode == code } ?? imported.values.first { $0.providerCode == code }
    }
}

/// Lifecycle of an on-device model file on this device.
enum OnDeviceModelStatus: Equatable {
    case absent
    case downloading(downloadedBytes: Int64, totalBytes: Int64)
    case ready
    case failed(String)

    var fraction: Double {
        if case let .downloading(downloaded, total) = self, total > 0 {
            return min(1, max(0, Double(downloaded) / Double(total)))
        }
        return 0
    }

    var isReady: Bool { if case .ready = self { return true }; return false }
}
