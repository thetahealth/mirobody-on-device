import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// One transcript row, mirroring chat.js appendMessage + appendFooter: a user
// turn as a right-aligned brand-tinted bubble (with a centered timestamp above
// it), or an assistant turn as rendered Markdown on the background -- optionally
// preceded by a dim "thinking" block and followed by a provider/stats/copy
// footer.
Item {
    id: del

    // Emitted when the row's height grows (streaming text), so the list can keep
    // the newest turn in view; and when the stats icon is tapped.
    signal contentGrew()
    signal statsClicked(var cost)

    implicitHeight: col.implicitHeight
    height: implicitHeight

    function pad(n) { return (n < 10 ? "0" : "") + n; }
    function formatLocalTime(ms) {
        var d = new Date(ms);
        if (isNaN(d.getTime())) return "";
        return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate())
             + " " + pad(d.getHours()) + ":" + pad(d.getMinutes());
    }
    function hasCost() {
        return model.cost && Object.keys(model.cost).length > 0;
    }

    Column {
        id: col
        width: parent.width
        spacing: 4
        onHeightChanged: del.contentGrew()

        // Timestamp above a user turn (introduces the Q&A pair).
        Label {
            visible: model.role === "user" && model.ts > 0
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: formatLocalTime(model.ts)
            color: Theme.surfaceVarFg
            opacity: 0.7
            font.pointSize: Theme.baseSize - 3
        }

        // Assistant "thinking" stream (dim italic), shown above the reply.
        Label {
            visible: model.role === "assistant" && model.thinking && model.thinking.length > 0
            width: parent.width
            text: model.thinking
            wrapMode: Text.Wrap
            font.italic: true
            font.pointSize: Theme.baseSize - 1
            color: Theme.surfaceVarFg
        }

        // User bubble (right-aligned, content-sized, capped width).
        Item {
            visible: model.role === "user"
            width: parent.width
            height: visible ? userBubble.height : 0
            Rectangle {
                id: userBubble
                anchors.right: parent.right
                color: Theme.userBubble
                radius: 14
                width: userText.contentWidth + 28
                height: userText.contentHeight + 20
                Text {
                    id: userText
                    x: 14; y: 10
                    width: del.width * 0.85 - 28
                    text: model.content
                    wrapMode: Text.Wrap
                    color: Theme.userBubbleText
                    font.pointSize: Theme.baseSize
                }
            }
        }

        // Assistant reply, rendered as Markdown.
        Text {
            visible: model.role === "assistant"
            width: parent.width
            text: model.content
            textFormat: Text.MarkdownText
            wrapMode: Text.Wrap
            color: Theme.surfaceFg
            font.pointSize: Theme.baseSize
            onLinkActivated: function (link) { Qt.openUrlExternally(link); }
        }

        // Footer: provider label + stats / copy actions (assistant only).
        RowLayout {
            visible: model.role === "assistant" && (del.hasCost() || (model.provider && model.provider.length > 0))
            width: parent.width
            spacing: 2

            Label {
                Layout.fillWidth: true
                text: model.provider || ""
                color: "#757575"
                font.pointSize: Theme.baseSize - 2
                elide: Text.ElideRight
            }
            ToolButton {
                visible: del.hasCost()
                text: "$"
                font.bold: true
                onClicked: del.statsClicked(model.cost)
            }
            ToolButton {
                text: "⧉"
                ToolTip.visible: hovered
                ToolTip.text: I18n.t("copyReply")
                onClicked: { clip.text = model.content; clip.selectAll(); clip.copy(); }
            }
        }
    }

    // Hidden helper to put the reply text on the clipboard (QML has no direct
    // clipboard API; TextEdit.copy() is the standard workaround).
    TextEdit { id: clip; visible: false }
}
