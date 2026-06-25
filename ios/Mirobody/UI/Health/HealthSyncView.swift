import SwiftUI

/// Drives the Apple Health sync sheet: requests HealthKit authorization, runs the
/// read→POST sync, and surfaces the result. Mirrors the Android `HealthViewModel`.
@MainActor
final class HealthSyncViewModel: ObservableObject {
    @Published var syncing = false
    @Published var status = ""

    private let repo: HealthKitRepository
    private let errorBus: ErrorBus

    init(repo: HealthKitRepository, errorBus: ErrorBus) {
        self.repo = repo
        self.errorBus = errorBus
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
                status = result.error ?? "Posted \(result.posted), failed \(result.failed)."
            } catch {
                errorBus.emit(error)
                status = error.localizedDescription
            }
            syncing = false
        }
    }
}

/// Settings sheet: read on-device Apple Health data and POST it as FHIR. Apple
/// HealthKit is on-device only (no cloud API), so this is the whole Apple path.
/// Labels are English literals for now — localize via the `.lproj` tables when
/// wiring translations (the iOS `L()` helper has no en fallback for missing keys).
struct HealthSyncView: View {
    @EnvironmentObject private var container: AppContainer
    @Environment(\.dismiss) private var dismiss
    @StateObject private var vm: HealthSyncViewModel

    init(container: AppContainer) {
        _vm = StateObject(wrappedValue: HealthSyncViewModel(
            repo: container.healthRepository,
            errorBus: container.errorBus,
        ))
    }

    var body: some View {
        VStack(spacing: 16) {
            Text("Sync health data").font(.headline)
            Text(vm.isAvailable
                 ? "Reads your activity, heart rate, sleep, and weight from Apple Health and adds them to your health record."
                 : "HealthKit is not available on this device.")
                .font(.subheadline)
                .multilineTextAlignment(.center)
                .foregroundColor(.secondary)

            if !vm.status.isEmpty {
                Text(vm.status).font(.footnote)
            }

            Button {
                vm.sync()
            } label: {
                Text(vm.syncing ? "Syncing…" : "Sync last 7 days")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(vm.syncing || !vm.isAvailable)

            Button("Close") { dismiss() }
        }
        .padding(24)
        .presentationDetents([.medium])
    }
}
