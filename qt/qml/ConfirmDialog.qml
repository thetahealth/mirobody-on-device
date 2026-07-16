import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// Generic confirm dialog (modals.showConfirmModal): title + message + a
// Cancel/confirm pair, the confirm tinted as destructive when `danger`. Emits
// confirmed() when the user proceeds.
Dialog {
    id: dialog

    property string message: ""
    property string confirmText: I18n.t("delete")
    property bool   danger: true

    signal confirmed()

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 360, 360)

    footer: DialogButtonBox {
        Button {
            text: I18n.t("cancel")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        Button {
            text: dialog.confirmText
            highlighted: true
            palette.button: dialog.danger ? Theme.error : Theme.primary
            palette.buttonText: "#ffffff"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }

    onAccepted: confirmed()

    contentItem: Label {
        text: dialog.message
        color: Theme.surfaceVarFg
        wrapMode: Text.WordWrap
    }
}
