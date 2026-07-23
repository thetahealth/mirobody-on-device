import Foundation

/// One on-device model the app can run. LiteRT-LM `.litertlm` files, edge-tuned and
/// published under the Apache-2.0 `litert-community` org (ungated, no token needed).
/// Each is downloaded at runtime (never bundled); several can coexist and the user
/// picks which to run. `providerCode` is the synthetic chat-provider id it surfaces as
/// in the picker. Mirrors Android's `data/llm/OnDeviceModel.kt`.
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

    var providerCode: String { OnDeviceModel.providerPrefix + id }
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
        OnDeviceModelSpec(
            id: "qwen3-0.6b",
            displayName: "Qwen3 0.6B (int4)",
            fileName: "qwen3-0.6b-int4.litertlm",
            downloadURL: URL(string:
                "https://huggingface.co/litert-community/Qwen3-0.6B/resolve/main/qwen3_0_6b_mixed_int4.litertlm?download=true")!,
            approxBytes: 497_664_000,
            recommendedRam: "4 GB+"
        ),
        OnDeviceModelSpec(
            id: "qwen2.5-1.5b",
            displayName: "Qwen2.5 1.5B",
            fileName: "qwen2.5-1.5b-instruct-q8.litertlm",
            downloadURL: URL(string:
                "https://huggingface.co/litert-community/Qwen2.5-1.5B-Instruct/resolve/main/Qwen2.5-1.5B-Instruct_multi-prefill-seq_q8_ekv4096.litertlm?download=true")!,
            approxBytes: 1_597_931_520,
            recommendedRam: "6 GB+"
        ),
        OnDeviceModelSpec(
            id: "qwen3-1.7b",
            displayName: "Qwen3 1.7B",
            fileName: "qwen3-1.7b.litertlm",
            downloadURL: URL(string:
                "https://huggingface.co/litert-community/Qwen3-1.7B/resolve/main/Qwen3_1.7B.litertlm?download=true")!,
            approxBytes: 2_056_729_520,
            recommendedRam: "6 GB+"
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
            id: "qwen3-4b-instruct",
            displayName: "Qwen3 4B (Instruct)",
            fileName: "qwen3-4b-instruct.litertlm",
            downloadURL: URL(string:
                "https://huggingface.co/litert-community/Qwen3-4B-Instruct-2507/resolve/main/qwen3_4b_instruct_2507_mixed_int4.litertlm?download=true")!,
            approxBytes: 2_659_057_664,
            recommendedRam: "8 GB+"
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

    static func byId(_ id: String) -> OnDeviceModelSpec? { catalog.first { $0.id == id } }
    static func byProviderCode(_ code: String) -> OnDeviceModelSpec? {
        catalog.first { $0.providerCode == code }
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
