import Foundation
import HealthKit

/// Outcome of a sync run: how many Observations the server accepted / rejected.
struct HealthSyncResult {
    let posted: Int
    let failed: Int
    let error: String?
}

/// Apple HealthKit on-device ingestion — the iOS sibling of Android's
/// `HealthRepository`. Reads HealthKit samples on the phone, maps each to a FHIR
/// R4 Observation, and POSTs it to the embedded server's /fhir endpoint (the bearer
/// token + base URL are supplied by `ApiClient`).
///
/// Requires the HealthKit capability and the `NSHealthShareUsageDescription` Info
/// string (added in project.yml). HealthKit is read on-device only — there is no
/// Apple cloud API — so this is the whole Apple integration.
@MainActor
final class HealthKitRepository {
    private let api: ApiClient
    private let store = HKHealthStore()

    init(api: ApiClient, settings: SettingsStore) {
        self.api = api
    }

    var isAvailable: Bool { HKHealthStore.isHealthDataAvailable() }

    private var readTypes: Set<HKObjectType> {
        [
            HKObjectType.quantityType(forIdentifier: .stepCount)!,
            HKObjectType.quantityType(forIdentifier: .heartRate)!,
            HKObjectType.quantityType(forIdentifier: .bodyMass)!,
            HKObjectType.categoryType(forIdentifier: .sleepAnalysis)!,
        ]
    }

    /// Prompt for read access to the four metrics. Call before `sync`. HealthKit
    /// never reveals whether read access was actually granted (privacy), so a
    /// subsequent empty read is indistinguishable from "no data".
    func requestAuthorization() async throws {
        guard isAvailable else {
            throw NSError(domain: "Health", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "HealthKit is not available on this device"])
        }
        try await store.requestAuthorization(toShare: [], read: readTypes)
    }

    /// Read [start, end] from HealthKit and POST each reading as a FHIR
    /// Observation. Posts are sequential; never throws on a single failed post.
    func sync(start: Date, end: Date) async throws -> HealthSyncResult {
        guard isAvailable else {
            return HealthSyncResult(posted: 0, failed: 0, error: "HealthKit is not available")
        }
        let samples = try await readAll(start: start, end: end)
        var posted = 0
        var failed = 0
        for sample in samples {
            do {
                let body = try FhirObservation.data(from: sample)
                try await api.postRaw("/fhir/Observation", jsonBody: body)
                posted += 1
            } catch {
                failed += 1
            }
        }
        return HealthSyncResult(posted: posted, failed: failed, error: nil)
    }

    // MARK: - Reading

    private func readAll(start: Date, end: Date) async throws -> [HealthSample] {
        let source = "Apple Health"
        let stepUnit = HKUnit.count()
        let bpmUnit = HKUnit.count().unitDivided(by: .minute())
        let kgUnit = HKUnit.gramUnit(with: .kilo)
        var out: [HealthSample] = []

        // Steps — count over each sample's interval.
        for q in try await quantitySamples(.stepCount, start, end) {
            out.append(HealthSample(metric: .steps, value: q.quantity.doubleValue(for: stepUnit),
                                    start: q.startDate, end: q.endDate, source: source))
        }
        // Heart rate — instantaneous bpm.
        for q in try await quantitySamples(.heartRate, start, end) {
            out.append(HealthSample(metric: .heartRate, value: q.quantity.doubleValue(for: bpmUnit),
                                    start: q.startDate, end: q.startDate, source: source))
        }
        // Weight — instantaneous body mass (kg).
        for q in try await quantitySamples(.bodyMass, start, end) {
            out.append(HealthSample(metric: .weight, value: q.quantity.doubleValue(for: kgUnit),
                                    start: q.startDate, end: q.startDate, source: source))
        }
        // Sleep — duration (min) of each asleep segment.
        for c in try await categorySamples(.sleepAnalysis, start, end) where isAsleep(c) {
            let minutes = c.endDate.timeIntervalSince(c.startDate) / 60.0
            out.append(HealthSample(metric: .sleep, value: minutes,
                                    start: c.startDate, end: c.endDate, source: source))
        }
        return out
    }

    private func isAsleep(_ sample: HKCategorySample) -> Bool {
        guard let value = HKCategoryValueSleepAnalysis(rawValue: sample.value) else { return false }
        switch value {
        case .asleepUnspecified, .asleepCore, .asleepDeep, .asleepREM:
            return true
        default:
            return false   // .inBed / .awake are not sleep time
        }
    }

    private func quantitySamples(
        _ id: HKQuantityTypeIdentifier, _ start: Date, _ end: Date
    ) async throws -> [HKQuantitySample] {
        let type = HKObjectType.quantityType(forIdentifier: id)!
        let predicate = HKQuery.predicateForSamples(withStart: start, end: end, options: .strictStartDate)
        return try await withCheckedThrowingContinuation { cont in
            let query = HKSampleQuery(sampleType: type, predicate: predicate,
                                      limit: HKObjectQueryNoLimit, sortDescriptors: nil) { _, samples, error in
                if let error { cont.resume(throwing: error); return }
                cont.resume(returning: (samples as? [HKQuantitySample]) ?? [])
            }
            store.execute(query)
        }
    }

    private func categorySamples(
        _ id: HKCategoryTypeIdentifier, _ start: Date, _ end: Date
    ) async throws -> [HKCategorySample] {
        let type = HKObjectType.categoryType(forIdentifier: id)!
        let predicate = HKQuery.predicateForSamples(withStart: start, end: end, options: .strictStartDate)
        return try await withCheckedThrowingContinuation { cont in
            let query = HKSampleQuery(sampleType: type, predicate: predicate,
                                      limit: HKObjectQueryNoLimit, sortDescriptors: nil) { _, samples, error in
                if let error { cont.resume(throwing: error); return }
                cont.resume(returning: (samples as? [HKCategorySample]) ?? [])
            }
            store.execute(query)
        }
    }
}
