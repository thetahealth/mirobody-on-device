import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// The chat view: the scrolling message log plus the composer. The provider
// picker lives in Main's top bar; here we render app.chat and send turns through
// app.sendMessage / app.stopStreaming. Mirrors chat.js buildChat.
Item {
    id: chatPage

    function showCost(cost) { costDialog.cost = cost; costDialog.open(); }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- message log ---------------------------------------------------
        ListView {
            id: log
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 14
            topMargin: 16
            bottomMargin: 16
            leftMargin: 16
            rightMargin: 16
            model: app.chat
            cacheBuffer: 4000
            boundsBehavior: Flickable.StopAtBounds

            delegate: MessageDelegate {
                width: ListView.view.width - 32
                onStatsClicked: function (cost) { chatPage.showCost(cost); }
                onContentGrew: if (index >= log.count - 1) log.positionViewAtEnd();
            }

            onCountChanged: positionViewAtEnd()

            // Empty state (chat.js showEmpty), shown before the first turn.
            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 360)
                spacing: 6
                visible: app.chat.count === 0
                Label {
                    text: I18n.t("emptyTitle")
                    font.pointSize: Theme.baseSize + 1
                    font.bold: true
                    color: Theme.onSurface
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width
                }
                Label {
                    text: I18n.t("emptySubtitle")
                    color: Theme.onSurfaceVar
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width
                }
            }
        }

        // --- composer ------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: composerRow.implicitHeight + 24
            color: Theme.background
            Rectangle { width: parent.width; height: 1; color: Theme.outlineVar } // top hairline

            RowLayout {
                id: composerRow
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12

                ScrollView {
                    Layout.fillWidth: true
                    Layout.maximumHeight: 140
                    Layout.alignment: Qt.AlignBottom

                    TextArea {
                        id: input
                        placeholderText: I18n.t("messageHint")
                        wrapMode: TextArea.Wrap
                        background: Rectangle {
                            radius: 16
                            color: Theme.surfaceLow
                            border.color: input.activeFocus ? Theme.primary : Theme.outlineVar
                            border.width: 1
                        }
                        // Enter sends; Shift+Enter inserts a newline (chat.js keydown).
                        Keys.onPressed: function (event) {
                            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                    && !(event.modifiers & Qt.ShiftModifier)) {
                                event.accepted = true;
                                chatPage.submit();
                            }
                        }
                    }
                }

                Button {
                    Layout.alignment: Qt.AlignBottom
                    highlighted: true
                    text: app.streaming ? I18n.t("close") : I18n.t("send")
                    onClicked: app.streaming ? app.stopStreaming() : chatPage.submit()
                }
            }
        }
    }

    function submit() {
        var text = input.text.trim();
        if (!text || app.streaming) return;
        app.sendMessage(text);
        input.clear();
    }

    CostDialog { id: costDialog }
}
