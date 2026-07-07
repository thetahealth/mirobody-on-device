import SwiftUI

/// Settings sheet for direct Bluetooth (BLE GATT) health sensors: scan, connect, and
/// stream a standard-profile sensor's readings into the FHIR store via
/// ``BleHealthController``. The iOS sibling of the desktop Qt BleDialog and Android
/// BleDeviceDialog. Core Bluetooth prompts for permission on the first scan (the
/// `NSBluetoothAlwaysUsageDescription` string in Info.plist).
///
/// Labels are English literals for now — localize via the `.lproj` tables when wiring
/// translations, like the sibling HealthSyncView.
struct BleDeviceView: View {
    @EnvironmentObject private var container: AppContainer
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var controller: BleHealthController

    init(container: AppContainer) {
        _controller = ObservedObject(wrappedValue: container.bleHealthController)
    }

    var body: some View {
        VStack(spacing: 12) {
            Text("Bluetooth health devices").font(.headline)
            Text("Connect a standard Bluetooth LE sensor (heart-rate strap, blood-pressure cuff, "
                + "thermometer). Readings are saved to your health record. Watches and rings use "
                + "private protocols — connect those under vendors.")
                .font(.footnote)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)

            List(controller.devices) { device in
                Button {
                    controller.connect(device.id)
                } label: {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(device.name)
                            Text(device.id).font(.caption2).foregroundColor(.secondary)
                        }
                        Spacer()
                        if device.supported {
                            Text("health").font(.caption2).foregroundColor(.accentColor)
                        }
                    }
                }
                .disabled(controller.connected)
            }
            .listStyle(.plain)
            .frame(maxHeight: 200)
            .overlay {
                if controller.devices.isEmpty {
                    Text("No devices yet — tap Scan")
                        .font(.footnote).foregroundColor(.secondary)
                }
            }

            if !controller.status.isEmpty {
                Text(controller.status).font(.footnote).foregroundColor(.secondary)
            }
            if let reading = controller.lastReading {
                Text(reading).font(.subheadline).bold()
            }
            if controller.posted > 0 || controller.failed > 0 {
                Text("Saved \(controller.posted), failed \(controller.failed)")
                    .font(.caption).foregroundColor(.secondary)
            }

            HStack(spacing: 12) {
                Button(controller.scanning ? "Stop" : "Scan") {
                    if controller.scanning { controller.stopScan() } else { controller.startScan() }
                }
                .buttonStyle(.borderedProminent)
                .disabled(controller.connected)

                if controller.connected {
                    Button("Disconnect") { controller.disconnect() }
                }
            }

            Button("Close") { controller.disconnect(); dismiss() }
        }
        .padding(24)
        .presentationDetents([.medium, .large])
    }
}
