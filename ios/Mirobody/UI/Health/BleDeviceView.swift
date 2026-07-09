import SwiftUI

/// Settings sheet for direct Bluetooth (BLE GATT) health sensors: scan, connect, and
/// stream a standard-profile sensor's readings into the FHIR store via
/// ``BleHealthController``. The iOS sibling of the desktop Qt BleDialog and Android
/// BleDeviceDialog. Core Bluetooth prompts for permission on the first scan (the
/// `NSBluetoothAlwaysUsageDescription` string in Info.plist).
struct BleDeviceView: View {
    @EnvironmentObject private var container: AppContainer
    @Environment(\.mbLanguage) private var lang
    @Environment(\.dismiss) private var dismiss
    @ObservedObject private var controller: BleHealthController

    init(container: AppContainer) {
        _controller = ObservedObject(wrappedValue: container.bleHealthController)
    }

    var body: some View {
        VStack(spacing: 12) {
            LText("ble_title").font(.headline)
            LText("ble_intro")
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
                            LText("ble_supported_tag").font(.caption2).foregroundColor(.accentColor)
                        }
                    }
                }
                .disabled(controller.connected)
            }
            .listStyle(.plain)
            .frame(maxHeight: 200)
            .overlay {
                if controller.devices.isEmpty {
                    LText("ble_no_devices")
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
                Text(L("ble_saved", lang, controller.posted, controller.failed))
                    .font(.caption).foregroundColor(.secondary)
            }

            HStack(spacing: 12) {
                Button(controller.scanning ? L("ble_stop", lang) : L("ble_scan", lang)) {
                    if controller.scanning { controller.stopScan() } else { controller.startScan() }
                }
                .buttonStyle(.borderedProminent)
                .disabled(controller.connected)

                if controller.connected {
                    Button(L("ble_disconnect", lang)) { controller.disconnect() }
                }
            }

            Button(L("common_close", lang)) { controller.disconnect(); dismiss() }
        }
        .padding(24)
        .presentationDetents([.medium, .large])
    }
}
