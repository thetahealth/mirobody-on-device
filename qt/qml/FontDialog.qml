import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// Font-size picker, mirroring modals.showFontSizeModal: a five-step slider over
// the FONT_TIERS, applied live through app.setFontOffset (Theme.baseSize follows
// app.fontOffset, so the whole UI -- including this dialog -- rescales).
Dialog {
    id: dialog
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 360, 360)
    title: I18n.t("fontSize")
    standardButtons: Dialog.Close

    function tierIndex(offset) {
        for (var i = 0; i < I18n.fontTiers.length; ++i)
            if (I18n.fontTiers[i][0] === offset) return i;
        return 2; // Normal
    }

    contentItem: ColumnLayout {
        spacing: 12
        Slider {
            id: slider
            Layout.fillWidth: true
            from: 0; to: I18n.fontTiers.length - 1; stepSize: 1; snapMode: Slider.SnapAlways
            value: dialog.tierIndex(app.fontOffset)
            onMoved: app.setFontOffset(I18n.fontTiers[Math.round(value)][0])
        }
        RowLayout {
            Layout.fillWidth: true
            Repeater {
                model: I18n.fontTiers
                delegate: Label {
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: I18n.t(modelData[1])
                    font.pointSize: Theme.baseSize - 3
                    color: Math.round(slider.value) === index ? Theme.primary : Theme.surfaceVarFg
                    font.bold: Math.round(slider.value) === index
                }
            }
        }
    }
}
