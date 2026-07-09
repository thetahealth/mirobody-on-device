import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// Connect my EHR (SMART on FHIR), mirroring ehr.js showEhrModal + Android
// EhrDialog. Search a provider directory or enter a FHIR base URL, then authorize
// (the server hands back an authorize_url that we open in the system browser; the
// OAuth callback completes server-side). Afterwards "Sync now" pulls Observations.
Dialog {
    id: dialog

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 460, 460)
    title: I18n.t("ehrConnectTitle")
    standardButtons: Dialog.Close

    // "" | "searching" | "connecting" | "error" | "synced"
    property string phase: ""
    property string message: ""
    property int    syncedCount: 0

    onAboutToShow: {
        phase = ""; message = ""; syncedCount = 0;
        results.clear(); searchField.clear(); manualField.clear();
    }

    ListModel { id: results }

    Connections {
        target: app
        function onEhrProvidersLoaded(providers) {
            results.clear();
            for (var i = 0; i < providers.length; ++i) {
                results.append({
                    name: providers[i].name || "",
                    fhirBaseUrl: providers[i].fhir_base_url || ""
                });
            }
            dialog.phase = "";
        }
        function onEhrError(msg) {
            dialog.phase = "error";
            dialog.message = msg && msg.length ? msg : I18n.t("ehrErrorMsg");
        }
        function onEhrConnecting() { dialog.phase = "connecting"; dialog.message = ""; }
        function onEhrSynced(posted) {
            dialog.phase = "synced";
            dialog.syncedCount = posted;
            dialog.message = I18n.t("ehrSyncDoneMsg", posted);
        }
    }

    contentItem: ColumnLayout {
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: I18n.t("ehrConnectSubtitle")
            color: Theme.onSurfaceVar
            wrapMode: Text.WordWrap
        }

        // --- Manual FHIR base URL -----------------------------------------
        Label {
            Layout.fillWidth: true
            text: I18n.t("ehrManualLabel")
            color: Theme.onSurface
            font.pointSize: Theme.baseSize - 1
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextField {
                id: manualField
                Layout.fillWidth: true
                placeholderText: "https://launch.smarthealthit.org/v/r4/fhir"
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhUrlCharactersOnly
            }
            Button {
                text: I18n.t("ehrConnectBtn")
                enabled: manualField.text.trim().length > 0 && dialog.phase !== "connecting"
                onClicked: { dialog.phase = "connecting"; app.ehrConnect(manualField.text); }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.outlineVar }

        // --- Directory search ---------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextField {
                id: searchField
                Layout.fillWidth: true
                placeholderText: I18n.t("ehrSearchPlaceholder")
                onAccepted: if (text.trim().length) { dialog.phase = "searching"; app.ehrSearchProviders(text); }
            }
            Button {
                text: I18n.t("ehrSearch")
                enabled: searchField.text.trim().length > 0 && dialog.phase !== "searching"
                onClicked: { dialog.phase = "searching"; app.ehrSearchProviders(searchField.text); }
            }
        }

        // --- Results ------------------------------------------------------
        Frame {
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            visible: dialog.phase === "searching" || results.count > 0
                     || (searchField.text.trim().length > 0)

            BusyIndicator {
                anchors.centerIn: parent
                running: dialog.phase === "searching"
                visible: running
            }
            Label {
                anchors.centerIn: parent
                visible: dialog.phase !== "searching" && results.count === 0
                         && searchField.text.trim().length > 0
                text: I18n.t("ehrNoResults")
                color: Theme.onSurfaceVar
            }
            ListView {
                anchors.fill: parent
                clip: true
                visible: results.count > 0
                model: results
                delegate: ItemDelegate {
                    width: ListView.view.width
                    required property string name
                    required property string fhirBaseUrl
                    height: col.implicitHeight + 16
                    onClicked: { dialog.phase = "connecting"; app.ehrConnect(fhirBaseUrl); }
                    contentItem: ColumnLayout {
                        id: col
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: name.length ? name : fhirBaseUrl
                            color: Theme.onSurface
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: fhirBaseUrl
                            color: Theme.onSurfaceVar
                            font.pointSize: Theme.baseSize - 2
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }
        }

        // --- Status line ---------------------------------------------------
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: text.length > 0
            color: dialog.phase === "error" ? Theme.error : Theme.onSurfaceVar
            text: {
                switch (dialog.phase) {
                case "searching":  return I18n.t("ehrSearching");
                case "connecting": return I18n.t("ehrConnecting");
                case "error":      return dialog.message;
                case "synced":     return dialog.message;
                default:           return "";
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.outlineVar }

        // --- Sync now ------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button {
                text: I18n.t("ehrSyncNow")
                highlighted: true
                onClicked: app.ehrSync()
            }
        }
    }
}
