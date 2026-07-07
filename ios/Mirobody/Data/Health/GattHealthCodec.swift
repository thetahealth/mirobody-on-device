import CoreBluetooth
import Foundation

/// Decodes standard SIG GATT health measurements into FHIR R4 `Observation` bodies —
/// the iOS counterpart of the desktop Qt `blehealth.cpp` and Android `GattHealthCodec`
/// decoders, producing the same LOINC/UCUM shapes the server's /fhir endpoint stores
/// (and matching ``FhirObservation``).
///
/// Supported today: Heart Rate `0x2A37`, Blood Pressure `0x2A35`, Temperature
/// `0x2A1C`. Adding one is a branch in ``decode(_:_:now:)``. Consumer watches/rings use
/// proprietary/encrypted GATT and are NOT decodable here — see src/health/README.md
/// ("Direct Bluetooth devices").
enum GattHealthCodec {

    // Standard 16-bit service/characteristic UUIDs (CoreBluetooth expands them onto
    // the Bluetooth base UUID; equality against a discovered 16-bit UUID holds).
    static let serviceHeartRate = CBUUID(string: "180D")
    static let serviceBloodPressure = CBUUID(string: "1810")
    static let serviceThermometer = CBUUID(string: "1809")
    static let supportedServices = [serviceHeartRate, serviceBloodPressure, serviceThermometer]

    static let chrHeartRate = CBUUID(string: "2A37")     // Heart Rate Measurement
    static let chrBloodPressure = CBUUID(string: "2A35") // Blood Pressure Measurement
    static let chrTemperature = CBUUID(string: "2A1C")   // Temperature Measurement
    static let supportedMeasurements = [chrHeartRate, chrBloodPressure, chrTemperature]

    /// One decoded reading: `value` in `unit` for the UI, plus the FHIR body to POST.
    struct Reading {
        let label: String
        let value: Double
        let unit: String
        let observation: Data
    }

    /// Decode one measurement notification into zero or more readings.
    static func decode(_ uuid: CBUUID, _ value: Data, now: Date) -> [Reading] {
        switch uuid {
        case chrHeartRate: return decodeHeartRate(value, now)
        case chrBloodPressure: return decodeBloodPressure(value, now)
        case chrTemperature: return decodeTemperature(value, now)
        default: return []
        }
    }

    // MARK: - decoders

    // Heart Rate Measurement (0x2A37): flags byte, then HR as uint8 or uint16 (LE).
    private static func decodeHeartRate(_ value: Data, _ now: Date) -> [Reading] {
        let b = [UInt8](value)
        guard b.count >= 2 else { return [] }
        let hr: Double
        if b[0] & 0x01 != 0 {                              // bit0: 16-bit value format
            guard b.count >= 3 else { return [] }
            hr = Double(Int(b[1]) | (Int(b[2]) << 8))
        } else {
            hr = Double(b[1])
        }
        return [Reading(
            label: "Heart rate", value: hr, unit: "bpm",
            observation: observation("8867-4", "Heart rate", "vital-signs", hr, true, "beats/minute", "/min", now),
        )]
    }

    // Blood Pressure Measurement (0x2A35): flags, then systolic/diastolic/MAP as SFLOAT.
    private static func decodeBloodPressure(_ value: Data, _ now: Date) -> [Reading] {
        let b = [UInt8](value)
        guard b.count >= 7 else { return [] }
        let kpa = b[0] & 0x01 != 0                         // bit0: 0 = mmHg, 1 = kPa
        let unit = kpa ? "kPa" : "mmHg"
        let ucum = kpa ? "kPa" : "mm[Hg]"
        func rd(_ off: Int) -> Double { sfloat(UInt16(b[off]) | (UInt16(b[off + 1]) << 8)) }
        var out: [Reading] = []
        func add(_ off: Int, _ loinc: String, _ display: String, _ label: String) {
            let n = rd(off)
            guard !n.isNaN else { return }
            out.append(Reading(
                label: label, value: n, unit: unit,
                observation: observation(loinc, display, "vital-signs", n, true, unit, ucum, now),
            ))
        }
        add(1, "8480-6", "Systolic blood pressure", "Systolic")
        add(3, "8462-4", "Diastolic blood pressure", "Diastolic")
        add(5, "8478-0", "Mean blood pressure", "Mean arterial")
        return out
    }

    // Temperature Measurement (0x2A1C): flags, then temperature as 32-bit FLOAT.
    private static func decodeTemperature(_ value: Data, _ now: Date) -> [Reading] {
        let b = [UInt8](value)
        guard b.count >= 5 else { return [] }
        let fahrenheit = b[0] & 0x01 != 0                  // bit0: 0 = Celsius, 1 = Fahrenheit
        let raw = UInt32(b[1]) | (UInt32(b[2]) << 8) | (UInt32(b[3]) << 16) | (UInt32(b[4]) << 24)
        let t = float32(raw)
        guard !t.isNaN else { return [] }
        return [Reading(
            label: "Temperature", value: t, unit: fahrenheit ? "°F" : "°C",
            observation: observation(
                "8310-5", "Body temperature", "vital-signs", t, false,
                fahrenheit ? "F" : "Cel", fahrenheit ? "[degF]" : "Cel", now,
            ),
        )]
    }

    // MARK: - IEEE-11073 floats

    // 16-bit SFLOAT: 4-bit signed exponent + 12-bit signed mantissa; special codes -> NaN.
    private static func sfloat(_ raw: UInt16) -> Double {
        let m = Int(raw & 0x0FFF)
        if m == 0x07FF || m == 0x0800 || m == 0x0801 || m == 0x0802 || m == 0x07FE { return .nan }
        var mantissa = m
        var exponent = Int(raw >> 12)
        if exponent >= 0x0008 { exponent -= 16 }           // signed 4-bit
        if mantissa >= 0x0800 { mantissa -= 4096 }         // signed 12-bit
        return Double(mantissa) * pow(10.0, Double(exponent))
    }

    // 32-bit FLOAT: 8-bit signed exponent + 24-bit signed mantissa.
    private static func float32(_ raw: UInt32) -> Double {
        var mantissa = Int(raw & 0x00FF_FFFF)
        let exponent = Int(Int8(bitPattern: UInt8((raw >> 24) & 0xFF)))  // signed 8-bit
        if mantissa >= 0x80_0000 { mantissa -= 0x0100_0000 }             // signed 24-bit
        return Double(mantissa) * pow(10.0, Double(exponent))
    }

    // MARK: - FHIR builder

    private static let iso: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f
    }()

    // Same Observation shape as FhirObservation.data(from:): LOINC code,
    // observation-category, UCUM valueQuantity, plus a Bluetooth provenance method.
    private static func observation(
        _ loinc: String, _ display: String, _ category: String,
        _ value: Double, _ integral: Bool, _ unit: String, _ ucum: String, _ now: Date,
    ) -> Data {
        let quantityValue: Any = integral ? Int(value.rounded()) : value
        let observation: [String: Any] = [
            "resourceType": "Observation",
            "status": "final",
            "category": [
                ["coding": [[
                    "system": "http://terminology.hl7.org/CodeSystem/observation-category",
                    "code": category,
                ]]],
            ],
            "code": [
                "coding": [[
                    "system": "http://loinc.org",
                    "code": loinc,
                    "display": display,
                ]],
            ],
            "effectiveDateTime": iso.string(from: now),
            "valueQuantity": [
                "value": quantityValue,
                "unit": unit,
                "system": "http://unitsofmeasure.org",
                "code": ucum,
            ],
            "method": [
                "coding": [["display": "Bluetooth LE"]],
            ],
        ]
        return (try? JSONSerialization.data(withJSONObject: observation, options: [])) ?? Data()
    }
}
