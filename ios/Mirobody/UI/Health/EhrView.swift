import SwiftUI

/// Backs the EHR-connect sheet: directory search, connect (open the SMART OAuth
/// URL in a browser), and sync (pull FHIR). Mirrors Android's `EhrViewModel`.
@MainActor
final class EhrViewModel: ObservableObject {
    @Published private(set) var results: [EhrProvider] = []
    @Published private(set) var searching = false
    @Published private(set) var busy = false          // connecting or syncing
    @Published private(set) var error: String?
    @Published private(set) var syncedCount: Int?      // non-nil after a successful sync
    /// One-shot OAuth URL for the sheet to open in a browser; cleared once launched.
    @Published var connectUrl: URL?

    private let repo: EhrRepository
    private let errorBus: ErrorBus
    private let language: String

    init(repo: EhrRepository, errorBus: ErrorBus, language: String) {
        self.repo = repo
        self.errorBus = errorBus
        self.language = language
    }

    func search(_ query: String) {
        let q = query.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return }
        searching = true
        error = nil
        syncedCount = nil
        Task {
            do {
                results = try await repo.providers(query: q)
            } catch {
                errorBus.emit(error)
                self.error = localizedMessage(error.toAppError(), language: language)
            }
            searching = false
        }
    }

    func connect(_ fhirBaseUrl: String) {
        var url = fhirBaseUrl.trimmingCharacters(in: .whitespaces)
        while url.hasSuffix("/") { url = String(url.dropLast()) }
        guard !url.isEmpty else { return }
        busy = true
        error = nil
        syncedCount = nil
        Task {
            do {
                if let authorize = try await repo.authorizeUrl(fhirBaseUrl: url), let u = URL(string: authorize) {
                    connectUrl = u
                } else {
                    error = localizedMessage(AppError.unknown(underlying: nil), language: language)
                }
            } catch {
                errorBus.emit(error)
                self.error = localizedMessage(error.toAppError(), language: language)
            }
            busy = false
        }
    }

    func sync() {
        busy = true
        error = nil
        syncedCount = nil
        Task {
            do {
                syncedCount = try await repo.sync()
            } catch {
                errorBus.emit(error)
                self.error = localizedMessage(error.toAppError(), language: language)
            }
            busy = false
        }
    }
}

/// Connect an EHR (SMART on FHIR): search the provider directory or paste a FHIR
/// base URL, connect (opens the SMART consent page in a browser), then Sync to pull
/// records. Mirrors Android's `EhrDialog` and the web client's EHR modal.
struct EhrView: View {
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    @StateObject private var vm: EhrViewModel
    @State private var manualUrl = ""
    @State private var query = ""

    init(container: AppContainer) {
        _vm = StateObject(wrappedValue: EhrViewModel(
            repo: container.ehrRepository,
            errorBus: container.errorBus,
            language: container.settings.language
        ))
    }

    var body: some View {
        NavigationStack {
            ZStack {
                colors.background.ignoresSafeArea()
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        LText("chat_ehr_subtitle")
                            .mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)

                        // Manual FHIR base URL (e.g. a SMART sandbox).
                        LText("chat_ehr_manual_label")
                            .mbFont(.bodySmall).foregroundColor(colors.onSurfaceVariant)
                        TextField("https://launch.smarthealthit.org/v/r4/fhir", text: $manualUrl)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .keyboardType(.URL)
                            .mbFont(.bodyLarge).foregroundColor(colors.onSurface)
                            .padding(12)
                            .overlay(RoundedRectangle(cornerRadius: 10)
                                .stroke(colors.outlineVariant, lineWidth: 1))
                        Button(L("chat_ehr_connect", lang)) { vm.connect(manualUrl) }
                            .disabled(manualUrl.trimmingCharacters(in: .whitespaces).isEmpty || vm.busy)
                            .foregroundColor(colors.primary)
                            .frame(maxWidth: .infinity, alignment: .trailing)

                        Divider().background(colors.outlineVariant)

                        // Directory search.
                        HStack(spacing: 8) {
                            TextField(L("chat_ehr_search_hint", lang), text: $query)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                                .mbFont(.bodyLarge).foregroundColor(colors.onSurface)
                                .padding(12)
                                .overlay(RoundedRectangle(cornerRadius: 10)
                                    .stroke(colors.outlineVariant, lineWidth: 1))
                                .onSubmit { vm.search(query) }
                            Button(L("chat_ehr_search", lang)) { vm.search(query) }
                                .disabled(query.trimmingCharacters(in: .whitespaces).isEmpty || vm.searching)
                                .foregroundColor(colors.primary)
                        }

                        if vm.searching {
                            ProgressView().frame(maxWidth: .infinity).padding(.vertical, 8)
                        } else {
                            ForEach(vm.results) { provider in
                                Button { vm.connect(provider.fhirBaseUrl) } label: {
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(provider.name.nonBlank ?? provider.fhirBaseUrl)
                                            .mbFont(.bodyMedium).foregroundColor(colors.onSurface)
                                            .lineLimit(1)
                                        Text(provider.fhirBaseUrl)
                                            .mbFont(.labelSmall).foregroundColor(colors.onSurfaceVariant)
                                            .lineLimit(1)
                                    }
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .padding(.horizontal, 12).padding(.vertical, 10)
                                }
                            }
                        }

                        if let error = vm.error {
                            Text(error).mbFont(.bodySmall).foregroundColor(colors.error)
                        } else if let count = vm.syncedCount {
                            Text(L("chat_ehr_sync_done", lang, count))
                                .mbFont(.bodySmall).foregroundColor(colors.onSurfaceVariant)
                        }

                        Divider().background(colors.outlineVariant)
                        Button(L("chat_ehr_sync_now", lang)) { vm.sync() }
                            .disabled(vm.busy)
                            .foregroundColor(colors.primary)
                            .frame(maxWidth: .infinity, alignment: .trailing)
                    }
                    .padding(20)
                }
            }
            .navigationTitle(L("chat_ehr_title", lang))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button { dismiss() } label: { Image(systemName: "xmark") }
                        .tint(colors.onSurfaceVariant)
                        .accessibilityLabel(L("common_close", lang))
                }
            }
        }
        // Hand a pending OAuth URL off to the system browser, then clear it.
        .onChange(of: vm.connectUrl) { url in
            guard let url else { return }
            openURL(url)
            vm.connectUrl = nil
        }
    }
}
