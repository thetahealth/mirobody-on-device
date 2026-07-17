import Foundation

/// The on-device model we ship support for: Gemma 4 E2B in LiteRT-LM format — the
/// edge-tuned variant Google publishes under the Apache-2.0 `litert-community` org
/// (ungated, no token needed). ~2.5 GB, downloaded at runtime, never bundled.
/// Mirrors Android's `data/llm/OnDeviceModel.kt`.
enum OnDeviceModel {
    static let displayName = "Gemma 4 E2B (instruction-tuned)"
    static let fileName = "gemma-4-E2B-it.litertlm"
    static let downloadURL = URL(
        string: "https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm/resolve/main/\(fileName)?download=true"
    )!
    /// Approximate size, for the UI to show before Content-Length lands.
    static let approxBytes: Int64 = 2_583 * 1024 * 1024
}

/// Lifecycle of the on-device model file on this device.
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
