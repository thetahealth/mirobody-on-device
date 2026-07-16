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

    // --- top bar (topbar.js buildTopBar: CenterAlignedTopAppBar) -----------
    // LEFT = account avatar that opens the nav drawer (a back arrow while adding
    // an account); CENTER = the optically-centered Mirobody brand; RIGHT = the
    // settings gear (app settings only). The provider/model picker now lives above
    // the composer in ChatPage, so the center stays the brand.
    header: ToolBar {
        height: 56
        background: Rectangle {
            color: Theme.background
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.outlineVar }
        }

        Item {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8

            // Left: account avatar (chat) / back arrow (adding account) / nothing.
            Item {
                id: leftZone
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: 40; height: 40

                // Account avatar: a filled circle with the first letter of the JWT
                // email, else a person glyph. Reads as "you / account" vs the gear.
                Rectangle {
                    id: avatar
                    anchors.centerIn: parent
                    visible: app.loggedIn && !app.addingAccount
                    width: 30; height: 30; radius: 15
                    color: Theme.primary
                    Label {
                        anchors.centerIn: parent
                        text: app.email.length > 0 ? app.email.charAt(0).toUpperCase() : "👤"
                        color: Theme.primaryFg
                        font.pointSize: Theme.baseSize - 1
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: { historyDrawer.reload(); historyDrawer.open(); }
                    }
                }

                // Back arrow: cancel Add-account and return to the current account.
                ToolButton {
                    anchors.fill: parent
                    visible: app.addingAccount
                    text: "←"
                    font.pointSize: Theme.baseSize + 4
                    onClicked: app.cancelAddAccount()
                }
            }

            // Center: the Mirobody brand (logo mark + serif-style wordmark), truly
            // centered. A drawn navy mark avoids depending on an external asset.
            Row {
                anchors.centerIn: parent
                spacing: 8
                visible: app.loggedIn && !app.addingAccount
                Rectangle {
                    width: 26; height: 26; radius: 6
                    color: Theme.brand
                    anchors.verticalCenter: parent.verticalCenter
                    Label {
                        anchors.centerIn: parent
                        text: "M"
                        color: Theme.primaryFg
                        font.bold: true
                        font.pointSize: Theme.baseSize
                    }
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Mirobody"
                    color: Theme.surfaceFg
                    font.pointSize: Theme.baseSize + 4
                    font.bold: true
                }
            }

            // Right: settings gear (app settings only; identical on login + chat).
            ToolButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
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
    // The login view is shown when signed out, or over an existing session while
    // adding a second account (mirrors app.js showLogin = addingAccount || !token).
    Loader {
        anchors.fill: parent
        sourceComponent: (app.loggedIn && !app.addingAccount) ? chatComponent : loginComponent
    }
    Component { id: loginComponent; LoginPage {} }
    Component { id: chatComponent;  ChatPage {} }

    // --- shared dialogs / drawer ------------------------------------------
    LanguageDialog { id: languageDialog }
    FontDialog     { id: fontDialog }
    BackendDialog  { id: backendDialog }
    BleDialog      { id: bleDialog }
    AboutDialog    { id: aboutDialog }
    EhrDialog      { id: ehrDialog }
    VendorsDialog  { id: vendorsDialog }
    HistoryDrawer  {
        id: historyDrawer
        // The Health & data group opens these from the drawer (moved off the gear).
        onOpenBle: bleDialog.open()
        onOpenEhr: ehrDialog.open()
        onOpenVendors: vendorsDialog.open()
    }
}
