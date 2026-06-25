import Foundation

/// A single on-device HealthKit reading, normalized for FHIR mapping. Mirrors the
/// Android `HealthSample` so both platforms post identical Observation shapes.
///
/// `value` is in `metric.ucumUnit`. Instantaneous readings (heart rate, weight)
/// set `start == end`; interval readings (a steps sample, a sleep segment) span
/// `[start, end)`.
struct HealthSample {
    let metric: HealthMetric
    let value: Double
    let start: Date
    let end: Date
    let source: String
}

/// The ingested metrics and the FHIR coding each maps to (kept in lockstep with
/// the Android `HealthMetric`).
enum HealthMetric {
    case steps
    case heartRate
    case sleep
    case weight

    var category: String {
        switch self {
        case .steps, .sleep: return "activity"
        case .heartRate, .weight: return "vital-signs"
        }
    }

    var loinc: String {
        switch self {
        case .steps: return "41950-7"
        case .heartRate: return "8867-4"
        case .sleep: return "93832-4"
        case .weight: return "29463-7"
        }
    }

    var display: String {
        switch self {
        case .steps: return "Number of steps in 24 hour Measured"
        case .heartRate: return "Heart rate"
        case .sleep: return "Sleep duration"
        case .weight: return "Body weight"
        }
    }

    var unit: String {
        switch self {
        case .steps: return "steps"
        case .heartRate: return "beats/minute"
        case .sleep: return "min"
        case .weight: return "kg"
        }
    }

    var ucumUnit: String {
        switch self {
        case .steps: return "{steps}"
        case .heartRate: return "/min"
        case .sleep: return "min"
        case .weight: return "kg"
        }
    }
}
