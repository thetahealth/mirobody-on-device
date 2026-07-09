import SwiftUI

/// Drives the Apple Health sync sheet: requests HealthKit authorization, runs the
/// read→POST sync, and surfaces the result. Mirrors the Android `HealthViewModel`.
@MainActor
final class HealthSyncViewModel: ObservableObject {
    @Published var syncing = false
    @Published var status = ""

    private let repo: HealthKitRepository
    private let errorBus: ErrorBus
    private let language: String

    init(repo: HealthKitRepository, errorBus: ErrorBus, language: String) {
        self.repo = repo
        self.errorBus = errorBus
        self.language = language
    }

    var isAvailable: Bool { repo.isAvailable }

    func sync(days: Int = 7) {
        syncing = true
        status = ""
        Task {
            do {
                try await repo.requestAuthorization()
                let end = Date()
                let start = Calendar.current.date(byAdding: .day, value: -days, to: end) ?? end
                let result = try await repo.sync(start: start, end: end)
                if let error = result.error {
                    status = L("health_error", language, error)
                } else {
                    status = L("health_result", language, result.posted, result.failed)
                }
            } catch {
                errorBus.emit(error)
                status = L("health_error", language, error.localizedDescription)
            }
            syncing = false
        }
    }
}

/// Settings sheet: read on-device Apple Health data and POST it as FHIR. Apple
/// HealthKit is on-device only (no cloud API), so this is the whole Apple path.
struct HealthSyncView: View {
    @EnvironmentObject private var container: AppContainer
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang
    @Environment(\.dismiss) private var dismiss
    @StateObject private var vm: HealthSyncViewModel

    init(container: AppContainer) {
        _vm = StateObject(wrappedValue: HealthSyncViewModel(
            repo: container.healthRepository,
            errorBus: container.errorBus,
            language: container.settings.language
        ))
    }

    var body: some View {
        VStack(spacing: 16) {
            LText("chat_sync_health").mbFont(.titleMedium).foregroundColor(colors.onSurface)
            Text(vm.isAvailable ? L("health_source", lang, "Apple Health") : L("health_no_source", lang))
                .mbFont(.bodyMedium)
                .multilineTextAlignment(.center)
                .foregroundColor(colors.onSurfaceVariant)

            if !vm.status.isEmpty {
                Text(vm.status).mbFont(.bodySmall).foregroundColor(colors.onSurface)
            }

            Button {
                vm.sync()
            } label: {
                Text(vm.syncing ? L("health_syncing", lang) : L("health_sync_button", lang))
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .tint(colors.primary)
            .disabled(vm.syncing || !vm.isAvailable)

            Button(L("common_close", lang)) { dismiss() }.foregroundColor(colors.primary)
        }
        .padding(24)
        .presentationDetents([.medium])
    }
}
