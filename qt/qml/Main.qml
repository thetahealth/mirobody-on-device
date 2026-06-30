import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// The orchestration hub, the QML analogue of app.js render(): a top bar (brand /
// provider picker / settings) above a Loader that swaps between the login and
// chat views on app.loggedIn. The settings dialogs and the history drawer live
// here so both the bar and the views can reach them.
ApplicationWindow {
    id: window
    visible: true
    width: 460
    height: 760
    minimumWidth: 360
    minimumHeight: 480
    title: "Mirobody"
    color: Theme.background
    font.pointSize: Theme.baseSize

    // Mirror the whole UI for right-to-left languages (Arabic / Hebrew).
    LayoutMirroring.enabled: I18n.isRtl(app.language)
    LayoutMirroring.childrenInherit: true

    // Keep the singletons in step with the persisted settings.
    Binding { target: Theme; property: "fontOffset"; value: app.fontOffset }
    Binding { target: I18n;  property: "language";   value: app.language }

    // --- top bar -----------------------------------------------------------
    header: ToolBar {
        background: Rectangle { color: Theme.background }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 8

            // Brand: opens the history drawer when signed in (as the web logo does).
            ToolButton {
                text: "Mirobody"
                font.pointSize: Theme.baseSize + 2
                font.bold: true
                flat: true
                enabled: app.loggedIn
                onClicked: { historyDrawer.reload(); historyDrawer.open(); }
                contentItem: Label {
                    text: "Mirobody"
                    font: parent.font
                    color: Theme.onSurface
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Item { Layout.fillWidth: true }

            // Provider picker (chat only). Restores the cached selection and
            // persists changes through app.setProvider.
            ComboBox {
                id: providerCombo
                visible: app.loggedIn
                Layout.maximumWidth: 220
                model: app.providers
                textRole: "name"
                valueRole: "name"
                displayText: currentIndex >= 0 ? currentText : I18n.t("selectModel")
                onActivated: {
                    app.setProvider(currentValue);
                    // On-device provider chosen but model not downloaded → prompt.
                    var p = app.providers[currentIndex];
                    if (p && p.code === "__ondevice_gemma4__" && app.onDeviceModel.status !== "ready")
                        onDeviceDialog.open();
                }
                Component.onCompleted: currentIndex = indexOfValue(app.provider)
                Connections {
                    target: app
                    function onProvidersChanged() {
                        providerCombo.currentIndex = providerCombo.indexOfValue(app.provider);
                    }
                    function onProviderChanged() {
                        providerCombo.currentIndex = providerCombo.indexOfValue(app.provider);
                    }
                }
            }

            Item { Layout.fillWidth: true }

            // Settings gear.
            ToolButton {
                text: "⚙"
                font.pointSize: Theme.baseSize + 4
                onClicked: settingsMenu.popup()
            }
        }
    }

    SettingsMenu {
        id: settingsMenu
        onOpenLanguage: languageDialog.open()
        onOpenFont: fontDialog.open()
        onOpenBackend: backendDialog.open()
        onOpenAbout: aboutDialog.open()
    }

    // --- body --------------------------------------------------------------
    Loader {
        anchors.fill: parent
        sourceComponent: app.loggedIn ? chatComponent : loginComponent
    }
    Component { id: loginComponent; LoginPage {} }
    Component { id: chatComponent;  ChatPage {} }

    // On-device model manager: explains the privacy trade-off and drives the
    // ~2.5 GB Gemma 4 download. Bound to app.onDeviceModel (ModelDownloader).
    Dialog {
        id: onDeviceDialog
        title: "On-device private AI"
        anchors.centerIn: parent
        modal: true
        width: Math.min(parent ? parent.width - 64 : 420, 420)
        standardButtons: Dialog.Close

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.onSurface
                text: "Gemma 4 runs entirely on your device. Your messages never leave " +
                      "the computer and work offline. This needs a one-time download of " +
                      "about 2.5 GB and enough free memory."
            }

            ProgressBar {
                Layout.fillWidth: true
                visible: app.onDeviceModel.status === "downloading"
                value: app.onDeviceModel.progress
            }

            Text {
                Layout.fillWidth: true
                color: app.onDeviceModel.status === "failed" ? "#c0392b" : Theme.onSurfaceVar
                text: {
                    switch (app.onDeviceModel.status) {
                    case "ready": return "Ready — runs offline.";
                    case "downloading": return Math.round(app.onDeviceModel.progress * 100) + "%";
                    case "failed": return "Download failed.";
                    default: return "Download required (~2.5 GB).";
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    text: app.onDeviceModel.status === "downloading" ? "Cancel download"
                        : (app.onDeviceModel.status === "failed" ? "Retry" : "Download model")
                    visible: app.onDeviceModel.status !== "ready"
                    onClicked: app.onDeviceModel.status === "downloading"
                        ? app.onDeviceModel.cancel() : app.onDeviceModel.start()
                }
                Button {
                    text: "Delete model"
                    visible: app.onDeviceModel.status === "ready"
                    onClicked: app.onDeviceModel.remove()
                }
                Item { Layout.fillWidth: true }
            }
        }
    }

    // --- shared dialogs / drawer ------------------------------------------
    LanguageDialog { id: languageDialog }
    FontDialog     { id: fontDialog }
    BackendDialog  { id: backendDialog }
    AboutDialog    { id: aboutDialog }
    HistoryDrawer  { id: historyDrawer }
}
