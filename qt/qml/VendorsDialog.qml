import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// Connected devices / vendors, mirroring vendors.js showVendorsModal + Android
// VendorsDialog. Two tabs (Devices / Platforms) over the catalog; each row shows
// the brand icon (server data-URI, monogram fallback), a "Pending" badge for an
// unverified link, and a Connect / Disconnect action. Connect opens the vendor's
// authorize_url in the system browser; Disconnect confirms then unlinks.
Dialog {
    id: dialog

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 460, 460)
    height: Math.min(parent ? parent.height - 64 : 560, 560)
    title: I18n.t("vendorManageTitle")
    standardButtons: Dialog.Close

    // Catalog (verbatim from vendors.js): the two groups, display names, and the
    // monogram background colors (fallback grey for ids without an explicit one).
    readonly property var deviceIds: [
        "oura", "whoop", "polar", "fitbit", "withings", "dexcom", "garmin", "huawei"
    ]
    readonly property var platformIds: [
        "terra", "validic", "rook", "spike", "junction", "wefitter", "thryve",
        "human_api", "vitalera", "metriport", "open_wearables", "redox",
        "particle_health", "healthconnect", "lexisnexis"
    ]
    readonly property var vendorNames: ({
        "oura": "Oura", "whoop": "WHOOP", "polar": "Polar", "fitbit": "Fitbit",
        "withings": "Withings", "dexcom": "Dexcom", "garmin": "Garmin", "huawei": "Huawei Health",
        "terra": "Terra", "validic": "Validic", "rook": "Rook", "spike": "Spike",
        "junction": "Junction", "wefitter": "WeFitter", "thryve": "Thryve", "human_api": "Human API",
        "vitalera": "Vitalera", "metriport": "Metriport", "open_wearables": "Open Wearables",
        "redox": "Redox", "particle_health": "Particle Health",
        "healthconnect": "HealthConnect CoPilot", "lexisnexis": "LexisNexis"
    })
    readonly property var vendorColors: ({
        "oura": "#4c6ef5", "whoop": "#0ca678", "polar": "#e8590c", "fitbit": "#0c8599",
        "withings": "#1c7ed6", "dexcom": "#7048e8", "garmin": "#2f9e44"
    })

    property int  tab: 0                 // 0 = Devices, 1 = Platforms
    property var  connected: ({})        // id -> { verified }
    property var  icons: ({})            // id -> dataURI
    property string errorMessage: ""
    property string pendingUnlink: ""

    function nameOf(id) { return vendorNames[id] || id; }
    function colorOf(id) { return vendorColors[id] || "#868e96"; }

    // The active tab's ids, sorted alphabetically by display name (vendors.js).
    function activeIds() {
        var ids = (tab === 0 ? deviceIds : platformIds).slice();
        ids.sort(function (a, b) { return nameOf(a).toLowerCase().localeCompare(nameOf(b).toLowerCase()); });
        return ids;
    }

    onAboutToShow: {
        errorMessage = "";
        connected = ({});
        icons = ({});
        app.loadVendors();
    }

    Connections {
        target: app
        function onVendorsLoaded(vendors) {
            var m = {};
            for (var i = 0; i < vendors.length; ++i) {
                var v = vendors[i];
                if (v && v.id) m[v.id] = { verified: v.verified === true };
            }
            dialog.connected = m;
        }
        function onVendorIconsLoaded(map) { dialog.icons = map; }
        function onVendorsError(msg) {
            dialog.errorMessage = msg && msg.length ? msg : I18n.t("ehrErrorMsg");
        }
        // Connect opened the browser / unlink finished: refresh the link list.
        function onVendorActionDone() { app.loadVendors(); }
    }

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: I18n.t("vendorManageSubtitle")
            color: Theme.onSurfaceVar
            wrapMode: Text.WordWrap
        }

        TabBar {
            id: tabBar
            Layout.fillWidth: true
            currentIndex: dialog.tab
            onCurrentIndexChanged: dialog.tab = currentIndex
            TabButton { text: I18n.t("vendorGroupDevices") }
            TabButton { text: I18n.t("vendorGroupPlatforms") }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dialog.activeIds()
            spacing: 4
            delegate: ItemDelegate {
                required property string modelData
                width: ListView.view.width
                height: 52
                // The row itself isn't a button; only the Connect/Disconnect is.
                background: null
                contentItem: RowLayout {
                    spacing: 10

                    // Icon: server data-URI when available, else a colored monogram.
                    Item {
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            visible: iconImage.status !== Image.Ready
                            color: dialog.colorOf(modelData)
                            Label {
                                anchors.centerIn: parent
                                text: dialog.nameOf(modelData).charAt(0).toUpperCase()
                                color: "#ffffff"
                                font.bold: true
                                font.pointSize: Theme.baseSize - 2
                            }
                        }
                        Image {
                            id: iconImage
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectFit
                            source: dialog.icons[modelData] || ""
                            visible: status === Image.Ready
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: dialog.nameOf(modelData)
                        color: Theme.onSurface
                        elide: Text.ElideRight
                    }

                    // "Pending" badge for a connected-but-unverified link.
                    Label {
                        visible: dialog.connected[modelData] !== undefined
                                 && !dialog.connected[modelData].verified
                        text: I18n.t("vendorPending")
                        color: Theme.onSurfaceVar
                        font.pointSize: Theme.baseSize - 2
                    }

                    // Connect / Disconnect.
                    Button {
                        enabled: true
                        readonly property bool isConnected: dialog.connected[modelData] !== undefined
                        text: isConnected ? I18n.t("vendorUnlink") : I18n.t("vendorConnect")
                        flat: isConnected
                        palette.buttonText: isConnected ? Theme.error : Theme.primary
                        onClicked: {
                            if (isConnected) {
                                dialog.pendingUnlink = modelData;
                                confirm.title = I18n.t("vendorUnlinkTitle");
                                confirm.message = I18n.t("vendorUnlinkMsg", dialog.nameOf(modelData));
                                confirm.confirmText = I18n.t("vendorUnlink");
                                confirm.open();
                            } else {
                                app.vendorConnect(modelData);
                            }
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: dialog.errorMessage.length > 0
            text: dialog.errorMessage
            color: Theme.error
            wrapMode: Text.WordWrap
        }
    }

    ConfirmDialog {
        id: confirm
        confirmText: I18n.t("vendorUnlink")
        onConfirmed: if (dialog.pendingUnlink.length) app.vendorUnlink(dialog.pendingUnlink)
    }
}
