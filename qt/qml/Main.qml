import QtQuick
import QtQuick.Controls
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

    // --- top bar (topbar.js buildTopBar) -----------------------------------
    // LEFT = the drawer hamburger with the Mirobody wordmark beside it (a back
    // arrow instead while adding an account). There is no right-hand zone: the
    // settings gear that used to fill it is a group inside the drawer now, so the
    // bar has one menu affordance rather than two competing ones. The
    // provider/model picker lives above the composer in ChatPage.
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

            // Left: the hamburger (a back arrow instead while adding an account).
            Item {
                id: leftZone
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: 40; height: 40

                // The one menu affordance, on every screen. It shows signed out too,
                // where the drawer narrows to the app-settings group -- that is how the
                // login screen reaches language / font / backend now that the gear is
                // gone. A nav glyph rather than the account avatar this used to be: the
                // drawer's body is the conversation list and account is one pinned row
                // at the bottom, so a profile icon undersold what the button opens.
                ToolButton {
                    anchors.fill: parent
                    visible: !app.addingAccount
                    text: "☰"
                    font.pointSize: Theme.baseSize + 4
                    onClicked: { historyDrawer.reload(); historyDrawer.open(); }
                }

                // Back arrow: cancel Add-account and return to the current account.
                // No drawer here -- it would be a dead end mid-flow.
                ToolButton {
                    anchors.fill: parent
                    visible: app.addingAccount
                    text: "←"
                    font.pointSize: Theme.baseSize + 4
                    onClicked: app.cancelAddAccount()
                }
            }

            // The wordmark, beside the hamburger. Chat only: the login view's own card
            // carries the mark and a display-size "Mirobody", so a second one in the
            // bar would state the brand twice on one screen. No logo mark next to it
            // either, matching the web bar.
            Label {
                anchors.left: leftZone.right
                anchors.leftMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                visible: app.loggedIn && !app.addingAccount
                text: "Mirobody"
                color: Theme.surfaceFg
                font.pointSize: Theme.baseSize + 4
                font.bold: true
            }
        }
    }

    // --- body --------------------------------------------------------------
    // The login view is shown when signed out, or over an existing session while
    // adding a second account (mirrors app.js showLogin = addingAccount || !token).
    Loader {
        anchors.fill: parent
        sourceComponent: (app.loggedIn && !app.addingAccount) ? chatComponent : loginComponent
    }
    Component { id: loginComponent; LoginPage {} }
    Component { id: chatComponent;  ChatPage { onManageOnDevice: onDeviceDialog.open() } }

    // --- the offscreen render host (formulas, charts) ----------------------
    // A hidden WebEngineView, in the window's tree but drawing nothing. Behind a
    // Loader with a STRING source so a build without Qt WebEngine -- where
    // render.canRender is false and RenderHostView.qml is not in the module -- never
    // resolves the type, let alone its `import QtWebEngine`.
    Loader {
        active: render.canRender
        source: "RenderHostView.qml"
    }

    // --- shared dialogs / drawer ------------------------------------------
    LanguageDialog { id: languageDialog }
    FontDialog     { id: fontDialog }
    BackendDialog  { id: backendDialog }
    BleDialog      { id: bleDialog }
    EhrDialog      { id: ehrDialog }
    VendorsDialog  { id: vendorsDialog }

    // On-device model manager (catalog + the user's own GGUFs). Shared: reached from
    // the provider picker AND the ⚙ menu.
    OnDeviceDialog { id: onDeviceDialog }

    HistoryDrawer  {
        id: historyDrawer
        // The Health & data group opens these from the drawer (moved off the gear).
        onOpenBle: bleDialog.open()
        onOpenEhr: ehrDialog.open()
        onOpenVendors: vendorsDialog.open()
        // ...and so does the app-settings group, which the gear used to hold. The
        // dialogs stay owned here so the drawer can close before one opens.
        onOpenLanguage: languageDialog.open()
        onOpenFont: fontDialog.open()
        onOpenBackend: backendDialog.open()
    }
}
