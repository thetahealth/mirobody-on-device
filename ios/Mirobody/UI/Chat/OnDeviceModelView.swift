import SwiftUI
import UniformTypeIdentifiers

/// Manage the on-device models: explains the privacy trade-off, then lists the catalog
/// (Gemma, Qwen, …) with a per-model (resumable) download / progress / delete, plus a
/// section for models imported from the Files app. A model appears in the provider picker
/// once it finishes downloading (or is imported). Mirrors Android's `OnDeviceModelDialog`.
/// Strings are inline English for now — localize via the `.lproj` tables in a follow-up.
struct OnDeviceModelView: View {
    let statuses: [String: OnDeviceModelStatus]
    let imported: [OnDeviceModelSpec]
    let onDownload: (OnDeviceModelSpec) -> Void
    let onPause: (OnDeviceModelSpec) -> Void
    let onDelete: (OnDeviceModelSpec) -> Void
    let onImport: (URL) -> Void
    let onDeleteImported: (OnDeviceModelSpec) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var showImporter = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Label("On-device private AI", systemImage: "lock.fill")
                    .font(.title3.bold())

                Text("Runs entirely on your device — private and offline. Download a model, or import a model file you already have.")
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

                HStack {
                    Text("Imported models").font(.headline)
                    Spacer()
                    Button { showImporter = true } label: {
                        Label("Import file", systemImage: "square.and.arrow.down")
                    }
                    .buttonStyle(.bordered)
                }
                if imported.isEmpty {
                    Text("Pick a .litertlm model file from Files. It stays where it is — outside the app — so it survives a reinstall.")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    ForEach(imported) { spec in
                        ImportedRow(spec: spec, onDelete: { onDeleteImported(spec) })
                        Divider()
                    }
                }

                Button { dismiss() } label: { Text("Done").frame(maxWidth: .infinity) }
                    .buttonStyle(.borderedProminent)
            }
            .padding()
        }
        .presentationDetents([.medium, .large])
        .fileImporter(
            isPresented: $showImporter,
            allowedContentTypes: [.data],
            allowsMultipleSelection: false
        ) { result in
            if case let .success(urls) = result, let url = urls.first { onImport(url) }
        }
    }
}

/// One imported-model row: name + size, with a "remove" action (keeps the user's file).
private struct ImportedRow: View {
    let spec: OnDeviceModelSpec
    let onDelete: () -> Void

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(spec.displayName).font(.headline)
                Text("~" + ByteCountFormatter.string(fromByteCount: spec.approxBytes, countStyle: .file)
                     + " · " + spec.fileName)
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer()
            Button(role: .destructive, action: onDelete) { Text("Remove") }
                .buttonStyle(.bordered)
        }
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
