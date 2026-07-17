import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// The chat view: an optional incognito banner, the scrolling message log, and the
// composer (with the provider/model picker sitting above it, following the web +
// Android layout). Renders app.chat and sends turns through app.sendMessage /
// app.stopStreaming. A conversation shared *to* the user is read-only, so the
// composer is hidden. Mirrors chat.js buildChat.
Item {
    id: chatPage

    // Ask Main to open the shared on-device model dialog (it lives there so the
    // ⚙ settings menu can reach it too).
    signal manageOnDevice()

    function showCost(cost) { costDialog.cost = cost; costDialog.open(); }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- incognito banner (pinned above the thread once it has turns) ---
        Rectangle {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.bottomMargin: 0
            visible: app.incognito && app.chat.count > 0
            implicitHeight: visible ? bannerRow.implicitHeight + 12 : 0
            radius: 10
            color: Theme.surfaceLow
            border.color: Theme.outlineVar
            border.width: 1
            RowLayout {
                id: bannerRow
                anchors.centerIn: parent
                spacing: 8
                Label { text: "🕵"; color: Theme.primary }
                Label {
                    text: I18n.t("incognitoNote")
                    color: Theme.surfaceVarFg
                    font.pointSize: Theme.baseSize - 1
                }
            }
        }

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

            // Follow the streaming reply's tail only while the user is parked at the
            // bottom. A manual drag/flick detaches the follow so they can scroll up
            // mid-reply without being yanked back; it re-arms when they leave the view
            // at the end, or when a new turn starts.
            property bool followTail: true
            onMovementStarted: followTail = false
            onMovementEnded: followTail = atYEnd

            delegate: MessageDelegate {
                width: ListView.view.width - 32
                onStatsClicked: function (cost) { chatPage.showCost(cost); }
                onContentGrew: if (index >= log.count - 1 && log.followTail) log.positionViewAtEnd();
            }

            onCountChanged: { followTail = true; positionViewAtEnd(); }

            // Empty state (chat.js showEmpty): the incognito privacy screen when
            // incognito, otherwise the plain "start a conversation" hero.
            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 360)
                spacing: 8
                visible: app.chat.count === 0
                Label {
                    visible: app.incognito
                    text: "🕵"
                    font.pointSize: Theme.baseSize + 28
                    color: Theme.primary
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width
                }
                Label {
                    text: app.incognito ? I18n.t("incognitoHeading") : I18n.t("emptyTitle")
                    font.pointSize: Theme.baseSize + 1
                    font.bold: true
                    color: Theme.surfaceFg
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width
                }
                Label {
                    text: app.incognito ? I18n.t("incognitoNote") : I18n.t("emptySubtitle")
                    color: Theme.surfaceVarFg
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    width: parent.width
                }
            }
        }

        // --- composer (hidden for a read-only shared conversation) ---------
        Rectangle {
            Layout.fillWidth: true
            visible: !app.readOnly
            implicitHeight: visible ? composerCol.implicitHeight + 24 : 0
            color: Theme.background
            Rectangle { width: parent.width; height: 1; color: Theme.outlineVar } // top hairline

            ColumnLayout {
                id: composerCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                // Provider / model picker (moved here from the top bar). Restores
                // the cached selection and persists changes through app.setProvider.
                ComboBox {
                    id: providerCombo
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: 380
                    // Grow to fit the widest model name (capped) instead of eliding
                    // it to "gemini-2.5-fla"; +60 leaves room for padding + the arrow.
                    Layout.preferredWidth: providerMetrics.width + 60
                    // Server + downloaded on-device providers (from C++), plus a
                    // trailing "Manage on-device AI" entry that opens the manager —
                    // mirrors the Electron/web picker. The label is translated here so
                    // it follows a language switch (I18n.language is a binding dep).
                    property var comboModel: {
                        var arr = [];
                        for (var i = 0; i < app.providers.length; ++i) arr.push(app.providers[i]);
                        arr.push({ name: I18n.t("manageModels"), code: "__ondevice_manage__" });
                        return arr;
                    }
                    model: comboModel
                    textRole: "name"
                    valueRole: "name"
                    flat: true
                    displayText: currentIndex >= 0 ? currentText : I18n.t("selectModel")
                    // Center the collapsed text; symmetric padding balances the
                    // right-side dropdown arrow so the text sits truly centered.
                    contentItem: Text {
                        text: providerCombo.displayText
                        font: providerCombo.font
                        color: Theme.surfaceFg
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        leftPadding: 28
                        rightPadding: 28
                    }
                    // Center each dropdown item too.
                    delegate: ItemDelegate {
                        width: providerCombo.width
                        highlighted: providerCombo.highlightedIndex === index
                        contentItem: Text {
                            text: modelData[providerCombo.textRole]
                            font: providerCombo.font
                            color: Theme.surfaceFg
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                    TextMetrics {
                        id: providerMetrics
                        font: providerCombo.font
                        text: {
                            var longest = I18n.t("selectModel");
                            var m = providerCombo.comboModel;
                            for (var i = 0; i < m.length; ++i) {
                                var n = m[i].name || "";
                                if (n.length > longest.length) longest = n;
                            }
                            return longest;
                        }
                    }
                    onActivated: {
                        var p = providerCombo.comboModel[currentIndex];
                        // "Manage on-device AI" isn't a real selection: revert to the
                        // current provider and open the manager.
                        if (p && p.code === "__ondevice_manage__") {
                            currentIndex = indexOfValue(app.provider);
                            chatPage.manageOnDevice();
                            return;
                        }
                        app.setProvider(currentValue);
                    }
                    Component.onCompleted: currentIndex = indexOfValue(app.provider)
                    Connections {
                        target: app
                        function onProvidersChanged() {
                            providerCombo.currentIndex = providerCombo.indexOfValue(app.provider);
                        }
                        function onProviderChanged() {
                            providerCombo.currentIndex = providerCombo.indexOfValue(app.provider);
                        }
                    }
                }

                RowLayout {
                    id: composerRow
                    Layout.fillWidth: true
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
    }

    function submit() {
        var text = input.text.trim();
        if (!text || app.streaming) return;
        app.sendMessage(text);
        input.clear();
    }

    CostDialog { id: costDialog }
}
