import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// The usage/cost summary, mirroring modals.showCostModal. `cost` is the map from
// the agent's costStatistics event.
Dialog {
    id: dialog
    property var cost: ({})

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 32 : 420, 420)
    title: I18n.t("statsTitle")
    standardButtons: Dialog.Close

    function field(k, d) { return (cost && cost[k] !== undefined) ? cost[k] : d; }

    contentItem: ColumnLayout {
        spacing: 6
        Repeater {
            model: [
                { label: I18n.t("statsModel"),       value: String(dialog.field("model", "")) },
                { label: I18n.t("statsInput"),       value: String(dialog.field("input_tokens", 0)) },
                { label: I18n.t("statsOutput"),      value: String(dialog.field("output_tokens", 0)) },
                { label: I18n.t("statsTotalTokens"), value: String(dialog.field("total_tokens", 0)) },
                { label: I18n.t("statsTotalCost"),   value: "$" + dialog.field("total_cost", 0) }
            ]
            delegate: RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label { text: modelData.label; color: Theme.surfaceVarFg }
                Item { Layout.fillWidth: true }
                Label {
                    text: modelData.value
                    color: Theme.surfaceFg
                    horizontalAlignment: Text.AlignRight
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
    }
}
