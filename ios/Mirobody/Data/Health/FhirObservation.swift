import Foundation

/// Turns a normalized ``HealthSample`` into a FHIR R4 `Observation` JSON body, the
/// shape the server's /fhir endpoint validates and stores. The server assigns the
/// id and meta, so only the clinical content is sent. Mirrors the Android
/// `FhirObservation`.
enum FhirObservation {

    private static let iso: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f
    }()

    /// Serialize `sample` to a FHIR Observation JSON body.
    static func data(from sample: HealthSample) throws -> Data {
        var observation: [String: Any] = [
            "resourceType": "Observation",
            "status": "final",
            "category": [
                ["coding": [[
                    "system": "http://terminology.hl7.org/CodeSystem/observation-category",
                    "code": sample.metric.category,
                ]]],
            ],
            "code": [
                "coding": [[
                    "system": "http://loinc.org",
                    "code": sample.metric.loinc,
                    "display": sample.metric.display,
                ]],
            ],
            "valueQuantity": [
                "value": sample.value,
                "unit": sample.metric.unit,
                "system": "http://unitsofmeasure.org",
                "code": sample.metric.ucumUnit,
            ],
            "method": [
                "coding": [["display": sample.source]],
            ],
        ]

        // Instantaneous vs interval timing.
        if sample.start == sample.end {
            observation["effectiveDateTime"] = iso.string(from: sample.start)
        } else {
            observation["effectivePeriod"] = [
                "start": iso.string(from: sample.start),
                "end": iso.string(from: sample.end),
            ]
        }

        return try JSONSerialization.data(withJSONObject: observation, options: [])
    }
}
