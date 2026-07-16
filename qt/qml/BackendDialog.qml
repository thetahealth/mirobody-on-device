import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// Backend base-URL editor, mirroring modals.showBackendModal: an editable field
// with the preset origins as suggestions, validated as http(s) or blank.
Dialog {
    id: dialog
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 440, 440)
    title: I18n.t("backendUrl")
    standardButtons: Dialog.Cancel | Dialog.Save

    onAboutToShow: { urlField.editText = app.baseUrl; errorLabel.visible = false; }
    onAccepted: {
        var v = urlField.editText.trim().replace(/\/+$/, "");
        if (v.length && !/^https?:\/\/\S+$/i.test(v)) {
            errorLabel.visible = true;
            open();              // reopen; the edit is invalid
            return;
        }
        app.setBaseUrl(v);
    }

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            Layout.fillWidth: true
            text: I18n.t("backendSubtitle")
            color: Theme.surfaceVarFg
            wrapMode: Text.WordWrap
        }
        ComboBox {
            id: urlField
            Layout.fillWidth: true
            editable: true
            model: app.baseUrlPresets
        }
        Label {
            id: errorLabel
            visible: false
            text: I18n.t("invalidUrl")
            color: Theme.error
            font.pointSize: Theme.baseSize - 1
        }
        Label {
            Layout.fillWidth: true
            text: I18n.t("backendHint")
            color: Theme.surfaceVarFg
            font.pointSize: Theme.baseSize - 1
            wrapMode: Text.WordWrap
        }
    }
}
