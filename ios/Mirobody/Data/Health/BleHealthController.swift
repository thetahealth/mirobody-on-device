import CoreBluetooth
import Foundation

/// Direct Bluetooth Low Energy (BLE GATT) health-sensor ingestion for iOS — the
/// mobile-native counterpart of the desktop Qt `BleHealth` and Android
/// `BleHealthController`. Scans for standard-profile sensors (HR strap, BP cuff,
/// thermometer) with Core Bluetooth, connects, subscribes to each supported
/// measurement characteristic, decodes it via ``GattHealthCodec``, and POSTs the FHIR
/// `Observation` to the server's /fhir endpoint — the same ingestion path HealthKit
/// uses. See src/health/README.md ("Direct Bluetooth devices").
///
/// On iOS this is the fallback for standard medical sensors with no companion app;
/// most consumer data still comes through HealthKit (``HealthKitRepository``).
///
/// Unlike Android, Core Bluetooth writes the CCCD and serializes GATT operations
/// itself, so `setNotifyValue` is all that's needed. The `CBCentralManager` is created
/// lazily on the first scan so the system Bluetooth prompt only appears when the user
/// opens this feature, not at launch. Delegate callbacks arrive on the main queue
/// (`queue: nil`), so `@Published` mutations are already main-thread.
@MainActor
final class BleHealthController: NSObject, ObservableObject {

    struct Device: Identifiable {
        let id: String        // peripheral.identifier.uuidString
        let name: String
        let supported: Bool
    }

    @Published private(set) var scanning = false
    @Published private(set) var connected = false
    @Published private(set) var status = ""
    @Published private(set) var devices: [Device] = []
    @Published private(set) var posted = 0
    @Published private(set) var failed = 0
    @Published private(set) var lastReading: String?

    private let api: ApiClient
    private var central: CBCentralManager?
    private var discovered: [String: CBPeripheral] = [:]
    private var peripheral: CBPeripheral?
    private var wantScan = false
    private var scanTimer: Timer?

    init(api: ApiClient) {
        self.api = api
        super.init()
    }

    // MARK: - Actions

    func startScan() {
        // First use: create the manager (this is what triggers the Bluetooth prompt);
        // the scan actually starts once it reports .poweredOn in didUpdateState.
        guard let central else {
            wantScan = true
            status = "Starting Bluetooth…"
            central = CBCentralManager(delegate: self, queue: nil)
            return
        }
        guard central.state == .poweredOn else {
            wantScan = true
            status = "Waiting for Bluetooth…"
            return
        }
        discovered.removeAll()
        devices = []
        scanning = true
        status = "Scanning…"
        // Unfiltered (adverts often omit service UUIDs); filtered in didDiscover.
        central.scanForPeripherals(withServices: nil, options: nil)
        scanTimer?.invalidate()
        scanTimer = Timer.scheduledTimer(withTimeInterval: 8, repeats: false) { [weak self] _ in
            Task { @MainActor in self?.stopScan() }
        }
    }

    func stopScan() {
        scanTimer?.invalidate(); scanTimer = nil
        if central?.state == .poweredOn { central?.stopScan() }
        if scanning {
            scanning = false
            status = devices.isEmpty ? "No devices found" : "Scan complete"
        }
    }

    func connect(_ id: String) {
        stopScan()
        guard let p = discovered[id] else { status = "Device not available"; return }
        posted = 0; failed = 0; lastReading = nil
        status = "Connecting…"
        peripheral = p
        p.delegate = self
        central?.connect(p, options: nil)
    }

    func disconnect() {
        if let p = peripheral { central?.cancelPeripheralConnection(p) }
        peripheral = nil
        connected = false
        status = "Disconnected"
    }

    // MARK: - Helpers

    private func post(_ observation: Data) {
        Task { @MainActor [weak self] in
            guard let self else { return }
            let ok = (try? await self.api.postRaw(
                "/fhir/Observation", jsonBody: observation, accept: "application/fhir+json")) != nil
            if ok { self.posted += 1 } else { self.failed += 1 }
        }
    }

    private func round1(_ v: Double) -> String {
        let r = (v * 10).rounded() / 10
        return r == r.rounded() ? String(Int(r)) : String(r)
    }
}

// MARK: - CBCentralManagerDelegate

extension BleHealthController: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if wantScan { wantScan = false; startScan() }
        case .poweredOff:
            scanning = false
            status = "Bluetooth is off"
        case .unauthorized:
            status = "Bluetooth permission denied"
        case .unsupported:
            status = "Bluetooth LE not supported on this device"
        default:
            break
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber,
    ) {
        let advUuids = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
        let supported = advUuids.contains { GattHealthCodec.supportedServices.contains($0) }
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name
        // Interesting if it advertises a supported service, or (fallback) has a name.
        if !supported && (name?.isEmpty ?? true) { return }
        let id = peripheral.identifier.uuidString
        if discovered[id] != nil { return }              // already listed
        discovered[id] = peripheral
        devices.append(Device(id: id, name: name ?? id, supported: supported))
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        status = "Discovering services…"
        peripheral.discoverServices(GattHealthCodec.supportedServices)
    }

    func centralManager(
        _ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?,
    ) {
        connected = false
        status = "Connection failed"
    }

    func centralManager(
        _ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?,
    ) {
        connected = false
        status = "Disconnected"
    }
}

// MARK: - CBPeripheralDelegate

extension BleHealthController: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        var any = false
        for service in peripheral.services ?? [] where GattHealthCodec.supportedServices.contains(service.uuid) {
            any = true
            peripheral.discoverCharacteristics(GattHealthCodec.supportedMeasurements, for: service)
        }
        if any { connected = true } else { status = "No supported health service on this device" }
    }

    func peripheral(
        _ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?,
    ) {
        for ch in service.characteristics ?? [] where GattHealthCodec.supportedMeasurements.contains(ch.uuid) {
            peripheral.setNotifyValue(true, for: ch)      // Core Bluetooth writes the CCCD
        }
        status = "Streaming readings…"
    }

    func peripheral(
        _ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?,
    ) {
        guard let value = characteristic.value else { return }
        for r in GattHealthCodec.decode(characteristic.uuid, value, now: Date()) {
            lastReading = "\(r.label): \(round1(r.value)) \(r.unit)"
            post(r.observation)
        }
    }
}
