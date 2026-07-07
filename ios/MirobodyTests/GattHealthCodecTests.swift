import XCTest
import CoreBluetooth
@testable import Mirobody

/// Unit tests for ``GattHealthCodec`` — the byte→reading→FHIR decoder, the iOS
/// sibling of the Android `GattHealthCodecTest` (same payloads, same expected
/// LOINC/UCUM/values). Exercises flag handling, uint8/uint16 heart rate, the SFLOAT
/// (blood pressure) and FLOAT (temperature) decoders — no Bluetooth hardware needed.
final class GattHealthCodecTests: XCTestCase {

    private let now = Date(timeIntervalSince1970: 0)
    private func bytes(_ v: [UInt8]) -> Data { Data(v) }

    // Navigate the emitted Observation JSON.
    private func json(_ data: Data) -> [String: Any] {
        (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
    }
    private func loinc(_ data: Data) -> String? {
        let obj = json(data)
        let code = obj["code"] as? [String: Any]
        let coding = (code?["coding"] as? [[String: Any]])?.first
        return coding?["code"] as? String
    }
    private func quantity(_ data: Data) -> [String: Any] {
        json(data)["valueQuantity"] as? [String: Any] ?? [:]
    }

    func testHeartRateUInt8() {
        // flags=0x00 (8-bit HR), value=72
        let r = GattHealthCodec.decode(GattHealthCodec.chrHeartRate, bytes([0x00, 0x48]), now: now)
        XCTAssertEqual(r.count, 1)
        XCTAssertEqual(r[0].value, 72, accuracy: 0)
        XCTAssertEqual(r[0].unit, "bpm")
        XCTAssertEqual(loinc(r[0].observation), "8867-4")
        XCTAssertEqual(quantity(r[0].observation)["code"] as? String, "/min")
        XCTAssertEqual(quantity(r[0].observation)["value"] as? Int, 72)   // integral
    }

    func testHeartRateUInt16() {
        // flags=0x01 (16-bit HR), value=320 (0x0140, little-endian)
        let r = GattHealthCodec.decode(GattHealthCodec.chrHeartRate, bytes([0x01, 0x40, 0x01]), now: now)
        XCTAssertEqual(r.count, 1)
        XCTAssertEqual(r[0].value, 320, accuracy: 0)
    }

    func testHeartRateTruncatedYieldsNothing() {
        XCTAssertTrue(GattHealthCodec.decode(GattHealthCodec.chrHeartRate, bytes([0x00]), now: now).isEmpty)
    }

    func testBloodPressureMmHgSFloat() {
        // flags=0x00 (mmHg); systolic=120 (0x0078), diastolic=80 (0x0050), MAP=93 (0x005D) as SFLOAT.
        let r = GattHealthCodec.decode(
            GattHealthCodec.chrBloodPressure,
            bytes([0x00, 0x78, 0x00, 0x50, 0x00, 0x5D, 0x00]), now: now)
        XCTAssertEqual(r.count, 3)
        XCTAssertEqual(r[0].value, 120, accuracy: 0)
        XCTAssertEqual(loinc(r[0].observation), "8480-6")   // systolic
        XCTAssertEqual(r[1].value, 80, accuracy: 0)
        XCTAssertEqual(loinc(r[1].observation), "8462-4")   // diastolic
        XCTAssertEqual(r[2].value, 93, accuracy: 0)
        XCTAssertEqual(loinc(r[2].observation), "8478-0")   // MAP
        XCTAssertEqual(r[0].unit, "mmHg")
        XCTAssertEqual(quantity(r[0].observation)["code"] as? String, "mm[Hg]")
    }

    func testTemperatureCelsiusFloat() {
        // flags=0x00 (Celsius); 32-bit FLOAT for 36.5 = mantissa 365 (0x00016D), exponent -1 (0xFF).
        let r = GattHealthCodec.decode(
            GattHealthCodec.chrTemperature, bytes([0x00, 0x6D, 0x01, 0x00, 0xFF]), now: now)
        XCTAssertEqual(r.count, 1)
        XCTAssertEqual(r[0].value, 36.5, accuracy: 1e-9)
        XCTAssertEqual(r[0].unit, "°C")
        XCTAssertEqual(loinc(r[0].observation), "8310-5")
        XCTAssertEqual(quantity(r[0].observation)["code"] as? String, "Cel")
        XCTAssertEqual(quantity(r[0].observation)["value"] as? Double, 36.5, accuracy: 1e-9)  // non-integral
    }

    func testTemperatureFahrenheitFlag() {
        // flags=0x01 (Fahrenheit); 98 = mantissa 98 (0x000062), exponent 0.
        let r = GattHealthCodec.decode(
            GattHealthCodec.chrTemperature, bytes([0x01, 0x62, 0x00, 0x00, 0x00]), now: now)
        XCTAssertEqual(r.count, 1)
        XCTAssertEqual(r[0].value, 98, accuracy: 0)
        XCTAssertEqual(r[0].unit, "°F")
        XCTAssertEqual(quantity(r[0].observation)["code"] as? String, "[degF]")
    }

    func testUnsupportedCharacteristicYieldsNothing() {
        // A non-measurement UUID → decode returns nothing.
        let r = GattHealthCodec.decode(CBUUID(string: "2902"), bytes([0x00, 0x01]), now: now)
        XCTAssertTrue(r.isEmpty)
    }
}
