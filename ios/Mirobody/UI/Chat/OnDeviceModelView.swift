import SwiftUI

/// Manage the on-device Gemma 4 model: explains the privacy trade-off, drives the
/// (resumable) download with progress, and offers delete. Mirrors the Android
/// `OnDeviceModelDialog`. Strings are inline English for now — localize via the
/// `.lproj` tables in a follow-up to match the rest of the app.
struct OnDeviceModelView: View {
    let status: OnDeviceModelStatus
    let onDownload: () -> Void
    let onPause: () -> Void
    let onDelete: () -> Void

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Label("On-device private AI", systemImage: "lock.fill")
                .font(.title3.bold())

            Text("Gemma 4 runs entirely on your device. Your messages never leave the phone and work offline. This needs a one-time download of about 2.5 GB and a device with enough memory.")
                .font(.callout)
                .foregroundStyle(.secondary)

            statusSection

            Spacer()

            controls
        }
        .padding()
        .presentationDetents([.medium])
    }

    @ViewBuilder private var statusSection: some View {
        switch status {
        case .downloading(let downloaded, let total):
            VStack(alignment: .leading, spacing: 6) {
                ProgressView(value: status.fraction)
                Text("\(formatBytes(downloaded)) / \(formatBytes(total))")
                    .font(.caption).foregroundStyle(.secondary)
            }
        case .failed(let message):
            Text(message).font(.caption).foregroundStyle(.red)
        case .ready:
            Label("Ready — runs offline", systemImage: "checkmark.circle.fill")
                .font(.callout).foregroundStyle(.green)
        case .absent:
            EmptyView()
        }
    }

    @ViewBuilder private var controls: some View {
        switch status {
        case .absent, .failed:
            Button(action: onDownload) {
                Text(status.isFailed ? "Retry download" : "Download model")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
        case .downloading:
            Button(action: onPause) { Text("Pause").frame(maxWidth: .infinity) }
                .buttonStyle(.bordered)
        case .ready:
            VStack(spacing: 8) {
                Button { dismiss() } label: { Text("Done").frame(maxWidth: .infinity) }
                    .buttonStyle(.borderedProminent)
                Button(role: .destructive, action: onDelete) {
                    Text("Delete model").frame(maxWidth: .infinity)
                }
            }
        }
    }

    private func formatBytes(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}

private extension OnDeviceModelStatus {
    var isFailed: Bool { if case .failed = self { return true }; return false }
}
