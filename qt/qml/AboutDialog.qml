import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// About box, mirroring modals.showAboutModal: the app name and build version.
Dialog {
    id: dialog
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 360, 360)
    title: I18n.t("about")
    standardButtons: Dialog.Close

    contentItem: ColumnLayout {
        spacing: 4
        Label {
            text: "Mirobody"
            font.bold: true
            font.pointSize: Theme.baseSize + 1
            color: Theme.onSurface
        }
        Label {
            text: I18n.t("version", app.appVersion)
            color: Theme.onSurfaceVar
        }
    }
}
