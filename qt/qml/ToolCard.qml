import QtQuick
import QtQuick.Controls
import Mirobody
import "markdown.js" as Md

// One tool invocation, above the reply it fed: a summary line that reads
// "Calling <tool>…" while the call runs and "Called <tool>" once it returns, and a
// body — collapsed by default — holding the arguments and the result as
// pretty-printed JSON. A port of the web client's <details> card (chat.js toolCard).
//
// Collapsed by default because the card answers "did it look anything up?" at a
// glance, and that is the whole question for almost every reader. The JSON is for the
// one who wants to check WHAT it looked up.
Item {
    id: card

    // { name, args, result, done } — see ChatModel::toolEvent.
    property var call: ({ name: "", args: "", result: "", done: false })

    property bool expanded: false

    implicitHeight: box.implicitHeight
    height: implicitHeight

    Column {
        id: box
        width: card.width
        spacing: 4

        // --- the summary line ------------------------------------------
        Item {
            width: parent.width
            height: summary.implicitHeight + 8

            Row {
                id: summary
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                spacing: 6

                // The disclosure triangle doubles as the running indicator: it spins
                // while the call is in flight. Two glyphs would say the same thing
                // twice, and a spinner that is also the control is one less thing in
                // a row that sits above every tool-using reply.
                Label {
                    id: chevron
                    text: card.expanded ? "▾" : "▸"
                    color: Theme.surfaceVarFg
                    font.pointSize: Theme.baseSize - 1

                    // No `opacity:` binding here on purpose. The pulse below is a VALUE
                    // SOURCE, and a value source REPLACES any binding on the property it
                    // drives -- so a `card.call.done ? 1.0 : 0.55` binding would be
                    // silently dropped and the chevron would keep whatever opacity the
                    // animation happened to stop on. It hands full opacity back itself
                    // when the call ends; the default is 1.0 until then.
                    SequentialAnimation on opacity {
                        running: !card.call.done
                        loops: Animation.Infinite
                        onRunningChanged: if (!running) chevron.opacity = 1.0
                        NumberAnimation { to: 1.0; duration: 500; easing.type: Easing.InOutQuad }
                        NumberAnimation { to: 0.35; duration: 500; easing.type: Easing.InOutQuad }
                    }
                }

                Label {
                    width: parent.width - chevron.width - parent.spacing
                    text: card.call.done
                          ? I18n.t("calledTool", card.call.name || "?")
                          : I18n.t("callingTool", card.call.name || "?")
                    // Italic while running, upright once it has returned -- the same
                    // "this is still happening" cue the web card uses.
                    font.italic: !card.call.done
                    font.pointSize: Theme.baseSize - 1
                    color: Theme.surfaceVarFg
                    elide: Text.ElideRight
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: card.expanded = !card.expanded
            }
        }

        // --- the body: arguments, then the result once there is one -----
        Rectangle {
            width: parent.width
            visible: card.expanded
            implicitHeight: visible ? bodyCol.implicitHeight + 12 : 0
            height: implicitHeight
            radius: 6
            color: Theme.surfaceLow
            border.color: Theme.outlineVar
            border.width: 1

            Column {
                id: bodyCol
                x: 8; y: 6
                width: parent.width - 16
                spacing: 2

                Label {
                    text: I18n.t("toolArguments")
                    font.bold: true
                    font.pointSize: Theme.baseSize - 2
                    color: Theme.surfaceFg
                }
                // Arguments stream in pieces, so a running call often holds half an
                // object; prettyJson returns it unchanged rather than hiding it until
                // it parses. Expanding a call in flight shows what has arrived.
                Label {
                    width: parent.width
                    text: Md.prettyJson(card.call.args || "{}")
                    wrapMode: Text.WrapAnywhere
                    font.family: "monospace"
                    font.pointSize: Theme.baseSize - 2
                    color: Theme.surfaceVarFg
                    // JSON stays left-to-right even in an RTL locale.
                    horizontalAlignment: Text.AlignLeft
                    LayoutMirroring.enabled: false
                }
                Label {
                    visible: (card.call.result || "").length > 0
                    text: I18n.t("toolResult")
                    font.bold: true
                    font.pointSize: Theme.baseSize - 2
                    color: Theme.surfaceFg
                }
                Label {
                    visible: (card.call.result || "").length > 0
                    width: parent.width
                    text: Md.prettyJson(card.call.result || "")
                    wrapMode: Text.WrapAnywhere
                    font.family: "monospace"
                    font.pointSize: Theme.baseSize - 2
                    color: Theme.surfaceVarFg
                    horizontalAlignment: Text.AlignLeft
                    LayoutMirroring.enabled: false
                }
            }
        }
    }
}
