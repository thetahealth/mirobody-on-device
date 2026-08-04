import SwiftUI

/// Backs the Connected-devices sheet: lists the static vendor catalog (devices /
/// platforms), marks the ones the server says are connected, and drives connect
/// (open the OAuth URL in a browser) / disconnect. Mirrors Android's
/// `VendorsViewModel` and the web `vendors.js`.
@MainActor
final class VendorsViewModel: ObservableObject {
    @Published private(set) var devices: [VendorItem] = []
    @Published private(set) var platforms: [VendorItem] = []
    @Published private(set) var loading = true
    @Published private(set) var error: String?
    /// One-shot OAuth URL for the sheet to open in a browser; cleared once launched.
    @Published var connectUrl: URL?

    private let repo: VendorRepository
    private let errorBus: ErrorBus
    private let language: String
    private var icons: [String: String] = [:]

    init(repo: VendorRepository, errorBus: ErrorBus, language: String) {
        self.repo = repo
        self.errorBus = errorBus
        self.language = language
    }

    func load() {
        loading = true
        error = nil
        Task {
            do {
                let connected = try await repo.connected()
                if icons.isEmpty { icons = await repo.icons() }
                build(connected)
            } catch {
                errorBus.emit(error)
                self.error = localizedMessage(error.toAppError(), language: language)
            }
            loading = false
        }
    }

    func connect(_ id: String) {
        Task {
            do {
                if let authorize = try await repo.authorizeUrl(id: id), let u = URL(string: authorize) {
                    connectUrl = u
                } else {
                    error = localizedMessage(AppError.unknown(underlying: nil), language: language)
                }
            } catch {
                errorBus.emit(error)
                self.error = localizedMessage(error.toAppError(), language: language)
            }
        }
    }

    func unlink(_ id: String) {
        Task {
            do {
                try await repo.unlink(id: id)
                load()
            } catch {
                errorBus.emit(error)
                self.error = localizedMessage(error.toAppError(), language: language)
            }
        }
    }

    private func build(_ connected: [String: VendorLink]) {
        func items(_ ids: [String]) -> [VendorItem] {
            ids.sorted { VendorCatalog.name(of: $0).lowercased() < VendorCatalog.name(of: $1).lowercased() }
                .map { id in
                    let link = connected[id]
                    return VendorItem(
                        id: id,
                        name: VendorCatalog.name(of: id),
                        connected: link != nil,
                        pending: link != nil && !(link?.verified ?? true),
                        iconDataUri: icons[id]
                    )
                }
        }
        devices = items(VendorCatalog.devices)
        platforms = items(VendorCatalog.platforms)
    }
}

/// Connected-devices manager: the vendor catalog in two tabs (Devices / Platforms),
/// each row Connect (opens the OAuth consent URL in a browser) or Disconnect. On
/// returning from the browser the list re-fetches, so a completed connect shows up.
/// Mirrors Android's `VendorsDialog`.
struct VendorsView: View {
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    @Environment(\.scenePhase) private var scenePhase

    @StateObject private var vm: VendorsViewModel
    @State private var activeTab = 0
    @State private var unlinkTarget: VendorItem?

    init(container: AppContainer) {
        _vm = StateObject(wrappedValue: VendorsViewModel(
            repo: container.vendorRepository,
            errorBus: container.errorBus,
            language: container.settings.language
        ))
    }

    private var items: [VendorItem] { activeTab == 0 ? vm.devices : vm.platforms }

    var body: some View {
        NavigationStack {
            ZStack {
                colors.background.ignoresSafeArea()
                VStack(alignment: .leading, spacing: 10) {
                    LText("chat_vendors_subtitle")
                        .mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)
                        .padding(.horizontal, 20)

                    Picker("", selection: $activeTab) {
                        LText("chat_vendor_devices").tag(0)
                        LText("chat_vendor_platforms").tag(1)
                    }
                    .pickerStyle(.segmented)
                    .padding(.horizontal, 16)

                    if vm.loading && items.isEmpty {
                        ProgressView().frame(maxWidth: .infinity).padding(24)
                    } else {
                        ScrollView {
                            LazyVStack(spacing: 6) {
                                ForEach(items) { item in
                                    VendorRow(item: item) {
                                        vm.connect(item.id)
                                    } onDisconnect: {
                                        unlinkTarget = item
                                    }
                                }
                            }
                            .padding(.horizontal, 16)
                        }
                    }

                    if let error = vm.error {
                        Text(error).mbFont(.bodySmall).foregroundColor(colors.error)
                            .padding(.horizontal, 20)
                    }
                    Spacer(minLength: 0)
                }
                .padding(.vertical, 12)
            }
            .navigationTitle(L("chat_vendors", lang))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button { dismiss() } label: { Image(systemName: "xmark") }
                        .tint(colors.onSurfaceVariant)
                        .accessibilityLabel(L("common_close", lang))
                }
            }
        }
        .onAppear { vm.load() }
        .onChange(of: vm.connectUrl) { url in
            guard let url else { return }
            openURL(url)
            vm.connectUrl = nil
        }
        // Re-list when the app returns to the foreground (e.g. after connecting in
        // the browser), so a completed connect shows up.
        .onChange(of: scenePhase) { phase in
            if phase == .active { vm.load() }
        }
        .confirmationDialog(
            L("chat_vendor_disconnect", lang),
            isPresented: Binding(get: { unlinkTarget != nil }, set: { if !$0 { unlinkTarget = nil } }),
            titleVisibility: .visible
        ) {
            Button(L("chat_vendor_disconnect", lang), role: .destructive) {
                if let target = unlinkTarget { vm.unlink(target.id) }
                unlinkTarget = nil
            }
            Button(L("common_cancel", lang), role: .cancel) { unlinkTarget = nil }
        } message: {
            if let target = unlinkTarget {
                Text(L("chat_vendor_unlink_confirm", lang, target.name))
            }
        }
    }
}

private struct VendorRow: View {
    let item: VendorItem
    let onConnect: () -> Void
    let onDisconnect: () -> Void
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    var body: some View {
        HStack(spacing: 10) {
            VendorIcon(item: item)
            Text(item.name).mbFont(.bodyMedium).foregroundColor(colors.onSurface).lineLimit(1)
            Spacer(minLength: 8)
            if item.pending {
                LText("chat_vendor_pending").mbFont(.labelSmall).foregroundColor(colors.onSurfaceVariant)
            }
            if item.connected {
                Button(L("chat_vendor_disconnect", lang), action: onDisconnect)
                    .buttonStyle(.bordered).tint(colors.error)
            } else {
                Button(L("chat_vendor_connect", lang), action: onConnect)
                    .buttonStyle(.bordered).tint(colors.primary)
            }
        }
        .padding(.horizontal, 12).padding(.vertical, 8)
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(colors.outlineVariant, lineWidth: 1))
    }
}

/// The brand's server-provided icon (data URI), or a colored monogram fallback.
private struct VendorIcon: View {
    let item: VendorItem
    @Environment(\.mbColors) private var colors

    var body: some View {
        if let uiImage = Self.decode(item.iconDataUri) {
            Image(uiImage: uiImage)
                .resizable().scaledToFit()
                .frame(width: 24, height: 24)
                .clipShape(RoundedRectangle(cornerRadius: 6))
        } else {
            ZStack {
                Circle().fill(colors.primary)
                Text(item.name.first.map { String($0).uppercased() } ?? "?")
                    .mbFont(.labelSmall).foregroundColor(colors.onPrimary)
            }
            .frame(width: 24, height: 24)
        }
    }

    /// Decode a "data:<mime>;base64,<…>" URI to a `UIImage`, or nil.
    private static func decode(_ uri: String?) -> UIImage? {
        guard let uri, let comma = uri.firstIndex(of: ",") else { return nil }
        let b64 = String(uri[uri.index(after: comma)...])
        guard let data = Data(base64Encoded: b64) else { return nil }
        return UIImage(data: data)
    }
}
