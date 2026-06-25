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
                onActivated: app.setProvider(currentValue)
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

    // --- shared dialogs / drawer ------------------------------------------
    LanguageDialog { id: languageDialog }
    FontDialog     { id: fontDialog }
    BackendDialog  { id: backendDialog }
    AboutDialog    { id: aboutDialog }
    HistoryDrawer  { id: historyDrawer }
}
