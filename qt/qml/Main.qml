import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
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

    // --- shared dialogs / drawer ------------------------------------------
    LanguageDialog { id: languageDialog }
    FontDialog     { id: fontDialog }
    BackendDialog  { id: backendDialog }
    BleDialog      { id: bleDialog }
    EhrDialog      { id: ehrDialog }
    VendorsDialog  { id: vendorsDialog }

    // On-device model manager: a list of GGUF models (remote download or local file);
    // one is active (●). Shared: reached from the provider picker AND the ⚙ menu.
    Dialog {
        id: onDeviceDialog
        title: I18n.t("onDeviceAi")
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(parent ? parent.width - 40 : 480, 480)
        standardButtons: Dialog.Close

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.surfaceVarFg
                font.pointSize: Theme.baseSize - 1
                text: I18n.t("onDeviceIntro")
            }

            // The model list.
            ListView {
                id: modelList
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 240)
                clip: true
                spacing: 2
                model: app.onDeviceModel.models
                delegate: RowLayout {
                    required property var modelData
                    width: ListView.view.width
                    spacing: 6

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            text: modelData.name
                            color: Theme.surfaceFg
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: Theme.baseSize - 3
                            color: Theme.surfaceVarFg
                            text: modelData.status === "ready" ? I18n.t("modelReady")
                                : modelData.status === "downloading"
                                    ? (I18n.t("modelDownloading") + " " + Math.round(modelData.progress * 100) + "%")
                                    : (modelData.remote ? I18n.t("modelNotDownloaded") : I18n.t("modelFileMissing"))
                        }
                    }
                    Button {
                        text: I18n.t("download")
                        visible: modelData.remote && modelData.status === "absent"
                        onClicked: app.onDeviceModel.download(modelData.name)
                    }
                    Button {
                        text: I18n.t("cancel")
                        visible: modelData.status === "downloading"
                        onClicked: app.onDeviceModel.cancel()
                    }
                    ToolButton {
                        id: delBtn
                        text: "✕"
                        // Red glyph so the destructive action reads as such (Fusion
                        // ToolButtons don't tint by role, so override the content).
                        contentItem: Text {
                            text: delBtn.text
                            color: Theme.error
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: modelData.remote ? I18n.t("deleteModel") : I18n.t("forgetModel")
                        // Deleting is destructive (a remote model's file is removed
                        // from disk; a local one is only forgotten) — confirm first.
                        onClicked: {
                            deleteConfirm.pendingName = modelData.name;
                            deleteConfirm.message = I18n.t(modelData.remote ? "deleteModelConfirm"
                                                                            : "forgetModelConfirm", modelData.name);
                            deleteConfirm.open();
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.outlineVar }

            // One-click add from a curated catalog (so users don't have to hunt for URLs).
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: app.onDeviceModel.suggestions.length > 0
                Label { text: I18n.t("suggested"); color: Theme.surfaceVarFg }
                ComboBox {
                    id: suggestCombo
                    Layout.fillWidth: true
                    model: app.onDeviceModel.suggestions
                    textRole: "label"
                }
                Button {
                    text: I18n.t("add")
                    onClicked: {
                        var s = app.onDeviceModel.suggestions[suggestCombo.currentIndex];
                        if (s) app.onDeviceModel.addRemote(s.name, s.url);
                    }
                }
            }

            // Or add any other GGUF by URL: name + URL.
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: addName
                    Layout.preferredWidth: 96
                    placeholderText: I18n.t("modelName")
                }
                TextField {
                    id: addUrl
                    Layout.fillWidth: true
                    placeholderText: "https://…/model.gguf"
                }
                Button {
                    text: I18n.t("add")
                    enabled: addUrl.text.trim().length > 0
                    onClicked: { app.onDeviceModel.addRemote(addName.text, addUrl.text); addName.clear(); addUrl.clear(); }
                }
            }

            Button {
                Layout.fillWidth: true
                text: I18n.t("addLocalFile")
                onClicked: modelFileDialog.open()
            }
        }
    }

    // Register an existing GGUF as a local model (no copy is made).
    FileDialog {
        id: modelFileDialog
        title: I18n.t("selectGgufModel")
        nameFilters: ["GGUF models (*.gguf)", "All files (*)"]
        onAccepted: app.onDeviceModel.addLocal("", selectedFile)
    }

    // Destructive-action confirm for the on-device model list (delete/forget).
    // The ✕ button fills in pendingName + message, then opens this.
    ConfirmDialog {
        id: deleteConfirm
        property string pendingName: ""
        title: I18n.t("deleteModel")
        confirmText: I18n.t("delete")
        onConfirmed: if (pendingName.length) app.onDeviceModel.remove(pendingName)
    }
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
