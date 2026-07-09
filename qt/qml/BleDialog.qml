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
    title: I18n.t("bleTitle")
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
            text: I18n.t("bleIntro")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                text: app.ble.scanning ? I18n.t("bleScanning") : I18n.t("bleScan")
                enabled: !app.ble.scanning
                onClicked: app.ble.startScan()
            }
            Button {
                text: I18n.t("bleStop")
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
                            text: I18n.t("bleSupportedTag")
                            color: Theme.primary
                            font.pointSize: Theme.baseSize - 2
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: list.count === 0
                    text: I18n.t("bleNoDevices")
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
            text: I18n.t("bleSaved", app.ble.posted) +
                  (app.ble.failed > 0 ? I18n.t("bleFailedSuffix", app.ble.failed) : "")
        }

        Button {
            text: I18n.t("bleDisconnect")
            visible: app.ble.connected
            onClicked: app.ble.disconnectDevice()
        }
    }
}
