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

    // Emitted when the stats icon is tapped. Keeping the newest turn in view is NOT
    // signalled from here: the row's height change is visible to the list as a
    // contentHeight change, and ChatPage follows the tail off that instead (a scroll
    // driven from inside the geometry change ran against the stale layout).
    signal statsClicked(var cost)

    implicitHeight: col.implicitHeight
    height: implicitHeight

    // Whether this row is the transcript's last. Read here and only here:
    // ListView.view is an attached property of the DELEGATE ROOT, so a nested
    // item asking for it gets undefined and the binding quietly evaluates false.
    readonly property bool isLastRow: ListView.view ? index === ListView.view.count - 1 : false

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

        // Waiting indicator: three dots holding the reply's place from the moment
        // the turn is sent until its first token. The send button becomes a stop
        // control at the same time, but that is at the other end of the window
        // from where the answer will appear -- and the wait is seconds long
        // whenever the model thinks or calls a tool first, which is exactly when
        // the user is watching this spot. The same indicator the web, Android and
        // iOS clients show.
        //
        // The condition is "the last row, assistant, still empty, while a turn is
        // running": the model carries no per-message streaming flag, and the only
        // row that can be mid-flight is the one at the tail.
        Row {
            id: dots
            visible: model.role === "assistant" && model.content.length === 0
                     && app.streaming && del.isLastRow
            height: visible ? Theme.baseSize * 2 : 0
            spacing: 4

            Repeater {
                model: 3
                Rectangle {
                    width: 6
                    height: 6
                    radius: 3
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.surfaceVarFg
                    opacity: 0.25

                    // Each dot runs the same cycle, offset by its index, so the
                    // pulse travels left to right. Bound to the row's visibility
                    // so it stops with the row rather than animating a hidden
                    // item for the life of the transcript.
                    SequentialAnimation on opacity {
                        running: dots.visible
                        loops: Animation.Infinite
                        PauseAnimation { duration: index * 160 }
                        NumberAnimation { to: 1.0; duration: 400; easing.type: Easing.InOutQuad }
                        NumberAnimation { to: 0.25; duration: 400; easing.type: Easing.InOutQuad }
                        PauseAnimation { duration: (2 - index) * 160 }
                    }
                }
            }
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
