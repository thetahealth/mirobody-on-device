import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// One transcript row, mirroring chat.js appendMessage + appendFooter and the
// Android MessageBubble: a user turn as a right-aligned navy bubble with a sharp
// "tail" corner (its timestamp tucked under it, right-aligned), or an assistant
// turn as rendered Markdown on the background -- optionally preceded by a dim
// "thinking" block and followed by a provider/stats/copy footer.
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

    // The row's data, hoisted onto the root.
    //
    // Not a convenience: `model` is a context property injected into the delegate, and
    // inside a Repeater it resolves to the REPEATER's own `model` instead. Everything
    // below that sits in one reads these.
    readonly property string content: model.content
    readonly property var    tools:   model.tools
    readonly property var    charts:  model.charts
    readonly property var    images:  model.images
    readonly property bool   isAssistant: model.role === "assistant"

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
        //
        // The shape is the web client's and Android's: 8px corners with a sharp 2px
        // "tail" at the top-inline-end corner, which is what marks the turn as the
        // user's without a second colour or an avatar. Rectangle carries ONE radius
        // before Qt 6.7 (per-corner radii landed there; this module's floor is 6.5),
        // so the tail is a patch -- a small square-ish rectangle of the same colour
        // laid over that corner, overriding the 8px arc with a 2px one.
        Item {
            visible: model.role === "user"
            width: parent.width
            height: visible ? userBubble.height : 0
            Rectangle {
                id: userBubble
                anchors.right: parent.right
                color: Theme.userBubble
                radius: 8
                width: userText.contentWidth + 28
                height: userText.contentHeight + 20
                Rectangle {
                    // The tail. LayoutMirroring flips `anchors.right` for RTL, so it
                    // follows the bubble to the top-inline-end corner by itself.
                    anchors.top: parent.top
                    anchors.right: parent.right
                    width: 8; height: 8
                    radius: 2
                    color: parent.color
                }
                Text {
                    id: userText
                    x: 14; y: 10
                    // Capped at the same 320px the web client and Android use, so a
                    // long turn wraps into a column rather than spanning the window.
                    width: Math.min(del.width * 0.85, 320) - 28
                    text: model.content
                    wrapMode: Text.Wrap
                    color: Theme.userBubbleText
                    font.pointSize: Theme.baseSize
                }
            }
        }

        // The user turn's timestamp, tucked UNDER the bubble and right-aligned
        // (chat.js appendMessage): it dates the exchange without breaking the
        // thread the way a full-width centered header does.
        Label {
            visible: model.role === "user" && model.ts > 0
            width: parent.width
            horizontalAlignment: Text.AlignRight
            text: formatLocalTime(model.ts)
            color: Theme.surfaceVarFg
            opacity: 0.7
            font.pointSize: Theme.baseSize - 3
        }

        // Tool invocations, above the reply: one expandable card per call, showing
        // "Calling <tool>…" while it runs and "Called <tool>" once it returns, with
        // the arguments and the result inside. Mirrors the web status block.
        Repeater {
            model: del.isAssistant ? del.tools : []
            ToolCard {
                required property var modelData
                width: col.width
                call: modelData
            }
        }

        // Assistant reply: prose, formulas, ```svg figures and ```echarts charts.
        ReplyBody {
            visible: del.isAssistant
            width: parent.width
            // Empty for a user turn rather than hidden-but-populated: the split and the
            // math scan would otherwise run on every user message too, for a body
            // nothing draws.
            source: del.isAssistant ? del.content : ""
        }

        // Charts the backend sent as their own `chart` events (an agent drawing a
        // trend), rather than as a fence inside the reply. Same renderer either way.
        Repeater {
            model: del.isAssistant ? del.charts : []
            ReplySegment {
                required property var modelData
                width: col.width
                segment: ({ kind: "chart", text: modelData })
            }
        }

        // Images the backend served (`image` events). Remote by nature, so they load
        // asynchronously and are capped to the column width.
        Repeater {
            model: del.isAssistant ? del.images : []
            Image {
                required property var modelData
                // Capped at the column, never upscaled past the picture's own size.
                // The height is DERIVED: with only a width set, an Image keeps its
                // natural height, so a wide picture squeezed into the column would
                // sit in a box far taller than the pixels it draws.
                width: Math.min(col.width, implicitWidth > 0 ? implicitWidth : col.width)
                height: implicitWidth > 0 ? width * implicitHeight / implicitWidth : 0
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                source: modelData
            }
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
                onClicked: {
                    clipboardHelper.text = model.content;
                    clipboardHelper.selectAll();
                    clipboardHelper.copy();
                }
            }
        }
    }

    // Hidden helper to put the reply text on the clipboard (QML has no direct
    // clipboard API; TextEdit.copy() is the standard workaround).
    //
    // NOT `id: clip`: every Item already has a `clip` property, and inside the
    // ToolButton above the name resolves to THAT bool -- scope-object properties
    // are looked up before component ids -- so the copy silently assigned to a
    // boolean instead of reaching this editor.
    TextEdit { id: clipboardHelper; visible: false }
}
