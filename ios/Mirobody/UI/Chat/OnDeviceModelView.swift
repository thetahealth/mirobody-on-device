import SwiftUI

/// Manage the on-device models: explains the privacy trade-off, then lists the catalog
/// (Gemma, Qwen, …) with a per-model (resumable) download / progress / delete. A model
/// appears in the provider picker once it finishes downloading. Mirrors Android's
/// `OnDeviceModelDialog`. Strings are inline English for now — localize via the `.lproj`
/// tables in a follow-up to match the rest of the app.
struct OnDeviceModelView: View {
    let statuses: [String: OnDeviceModelStatus]
    let onDownload: (OnDeviceModelSpec) -> Void
    let onPause: (OnDeviceModelSpec) -> Void
    let onDelete: (OnDeviceModelSpec) -> Void

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Label("On-device private AI", systemImage: "lock.fill")
                .font(.title3.bold())

            Text("Runs entirely on your device — private and offline. Each model is a one-time download.")
                .font(.callout)
                .foregroundStyle(.secondary)

            ForEach(OnDeviceModel.catalog) { spec in
                ModelRow(
                    spec: spec,
                    status: statuses[spec.id] ?? .absent,
                    onDownload: { onDownload(spec) },
                    onPause: { onPause(spec) },
                    onDelete: { onDelete(spec) }
                )
                Divider()
            }

            Spacer()

            Button { dismiss() } label: { Text("Done").frame(maxWidth: .infinity) }
                .buttonStyle(.borderedProminent)
        }
        .padding()
        .presentationDetents([.medium, .large])
    }
}

/// One catalog row: model name + its download / progress / delete affordance.
private struct ModelRow: View {
    let spec: OnDeviceModelSpec
    let status: OnDeviceModelStatus
    let onDownload: () -> Void
    let onPause: () -> Void
    let onDelete: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(spec.displayName).font(.headline)
                    Text("~" + formatBytes(spec.approxBytes) + " · " + spec.recommendedRam + " RAM")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                controls
            }
            statusSection
        }
    }

    @ViewBuilder private var statusSection: some View {
        switch status {
        case .downloading(let downloaded, let total):
            VStack(alignment: .leading, spacing: 4) {
                ProgressView(value: status.fraction)
                Text("\(formatBytes(downloaded)) / \(formatBytes(total))")
                    .font(.caption).foregroundStyle(.secondary)
            }
        case .failed(let message):
            Text(message).font(.caption).foregroundStyle(.red)
        case .ready:
            Label("Ready — runs offline", systemImage: "checkmark.circle.fill")
                .font(.caption).foregroundStyle(.green)
        case .absent:
            EmptyView()
        }
    }

    @ViewBuilder private var controls: some View {
        switch status {
        case .absent, .failed:
            Button(action: onDownload) { Text(status.isFailed ? "Retry" : "Download") }
                .buttonStyle(.bordered)
        case .downloading:
            Button(action: onPause) { Text("Pause") }
                .buttonStyle(.bordered)
        case .ready:
            Button(role: .destructive, action: onDelete) { Text("Delete") }
                .buttonStyle(.bordered)
        }
    }

    private func formatBytes(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}

private extension OnDeviceModelStatus {
    var isFailed: Bool { if case .failed = self { return true }; return false }
}
