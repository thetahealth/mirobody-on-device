import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// Direct Bluetooth (BLE GATT) health-sensor connect. Scans for standard-profile
// sensors (HR strap, BP cuff, thermometer), connects, and streams their readings
// straight to the server's FHIR store via app.ble (BleHealth). The desktop
// counterpart of the mobile "Connect health data" flow. Strings are inline (like
// the on-device dialog), since this is desktop-only.
Dialog {
    id: dialog

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 460, 460)
    title: "Bluetooth health devices"
    standardButtons: Dialog.Close
    onClosed: app.ble.disconnectDevice()

    // Latest decoded reading, shown as a live readout.
    property string lastReading: ""
    Connections {
        target: app.ble
        function onReading(label, value, unit) {
            dialog.lastReading = label + ": " + Math.round(value * 10) / 10 + " " + unit;
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.onSurfaceVar
            text: "Connect a standard Bluetooth LE sensor (heart-rate strap, blood-" +
                  "pressure cuff, thermometer). Readings are saved to your health record. " +
                  "Watches and rings use private protocols — connect those under vendors."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                text: app.ble.scanning ? "Scanning…" : "Scan"
                enabled: !app.ble.scanning
                onClicked: app.ble.startScan()
            }
            Button {
                text: "Stop"
                enabled: app.ble.scanning
                onClicked: app.ble.stopScan()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                running: app.ble.scanning
                implicitWidth: 22; implicitHeight: 22
            }
        }

        // Discovered devices; supported ones (advertise a known health service)
        // are marked and listed for one-tap connect.
        Frame {
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            ListView {
                id: list
                anchors.fill: parent
                clip: true
                model: app.ble.devices
                delegate: ItemDelegate {
                    width: ListView.view.width
                    enabled: !app.ble.connected
                    onClicked: app.ble.connectDevice(index)
                    contentItem: RowLayout {
                        spacing: 8
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label { text: modelData.name; color: Theme.onSurface }
                            Label {
                                text: modelData.id
                                color: Theme.onSurfaceVar
                                font.pointSize: Theme.baseSize - 2
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                        Label {
                            visible: modelData.supported
                            text: "health"
                            color: Theme.primary
                            font.pointSize: Theme.baseSize - 2
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: list.count === 0
                    text: "No devices yet — tap Scan"
                    color: Theme.onSurfaceVar
                }
            }
        }

        // Status + live reading + running FHIR post tally.
        Label {
            Layout.fillWidth: true
            color: Theme.onSurfaceVar
            text: app.ble.status
        }
        Label {
            Layout.fillWidth: true
            visible: dialog.lastReading.length > 0
            color: Theme.onSurface
            font.bold: true
            text: dialog.lastReading
        }
        Label {
            Layout.fillWidth: true
            visible: app.ble.posted > 0 || app.ble.failed > 0
            color: Theme.onSurfaceVar
            text: "Saved " + app.ble.posted + " reading(s)" +
                  (app.ble.failed > 0 ? ", " + app.ble.failed + " failed" : "")
        }

        Button {
            text: "Disconnect"
            visible: app.ble.connected
            onClicked: app.ble.disconnectDevice()
        }
    }
}
