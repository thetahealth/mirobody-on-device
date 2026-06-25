import QtQuick
import QtQuick.Controls
import Mirobody

// Language picker, mirroring modals.showLanguageModal: the ten languages, the
// current one highlighted. Picking one applies it through app.setLanguage,
// which re-evaluates every I18n.t() binding.
Dialog {
    id: dialog
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 360, 360)
    height: Math.min(parent ? parent.height - 80 : 480, 480)
    title: I18n.t("language")
    standardButtons: Dialog.Close

    contentItem: ListView {
        clip: true
        model: I18n.languages
        delegate: ItemDelegate {
            width: ListView.view.width
            required property var modelData
            highlighted: modelData[0] === app.language
            contentItem: Label {
                text: modelData[1]
                color: highlighted ? Theme.primary : Theme.onSurface
                font.bold: highlighted
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: { app.setLanguage(modelData[0]); dialog.close(); }
        }
    }
}
