import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody
import "slash.js" as Slash

// The chat view: an optional incognito banner, the scrolling message log, and the
// composer. Renders app.chat and sends turns through app.sendMessage /
// app.stopStreaming. A conversation shared *to* the user is read-only, so the
// composer is hidden. Mirrors chat.js buildChat and Android's ChatScreen.
Item {
    id: chatPage

    // Ask Main to open the shared on-device model dialog (it lives there so the
    // ⚙ settings menu can reach it too).
    signal manageOnDevice()

    // Escape closed the slash palette; the next keystroke reopens it (chat.js
    // `paletteHidden`). Lives on the page, not the palette, so a dismissal
    // survives the palette being destroyed and rebuilt as the draft changes.
    property bool paletteHidden: false

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

            // Sticky bottom, mirroring Index.ets: follow the growing tail only while
            // the user is parked at the end, and let go the moment they scroll away so
            // reading back never fights the stream. Re-arms by itself when they return
            // to the bottom, and on every new turn.
            property bool followTail: true

            /**
             * One handler covers every way the log gets taller: a turn appended, the
             * streaming row growing token by token, the composer growing under it, the
             * window resized. All of them move the bottom, and contentHeight/height are
             * where that shows up.
             *
             * DEFERRED on purpose. positionViewAtEnd() called straight out of a delegate's
             * geometry change runs against the pre-change layout, so it lands one growth
             * step short -- measured against a 30-turn transcript, the newest line sat a
             * constant ~18px (one line) below the fold for the whole reply, and the
             * repeated mid-layout calls also corrupted the view's own height estimates.
             * Qt.callLater runs it after layout settles and collapses a burst of deltas
             * into a single scroll.
             */
            onContentHeightChanged: if (followTail) Qt.callLater(stickToEnd)
            onHeightChanged: if (followTail) Qt.callLater(stickToEnd)
            function stickToEnd() { if (followTail) positionViewAtEnd(); }

            /**
             * Whether the newest row is fully in view -- the question the re-arm needs
             * answered, asked directly.
             *
             * NOT Flickable's atYEnd: positionViewAtEnd() parks the last row's bottom edge
             * on the viewport bottom and leaves `bottomMargin` of content area below it,
             * so atYEnd reads false even while we are pinned. Using it as the re-arm test
             * meant a single wheel notch stopped the follow for the rest of the session.
             * A null item means the tail is not even realized, i.e. far off-screen.
             */
            function atBottom() {
                var it = itemAtIndex(count - 1);
                return it ? it.y + it.height <= contentY + height + 2 : false;
            }

            // Only a finger/wheel gesture may detach the follow; positionViewAtEnd()
            // sets the position outright and emits no movement signals, so it cannot
            // undo the stick it just honored.
            onMovementStarted: followTail = false
            onMovementEnded: followTail = atBottom()

            delegate: MessageDelegate {
                width: ListView.view.width - 32
                onStatsClicked: function (cost) { chatPage.showCost(cost); }
            }

            // A new turn is always something the user just asked for, so it re-arms the
            // follow even if they had scrolled up to read. Also covers a cleared thread
            // and a session restored from history (both reset the model).
            onCountChanged: { followTail = true; Qt.callLater(stickToEnd); }

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

                // --- slash-command palette, above the pill -------------------
                //
                // The discovery mechanism -- nobody is expected to know these words
                // in advance -- so it opens on a lone "/" and narrows by prefix.
                // Clicking a row runs the command straight away: every one of them
                // is also a control in the drawer, so there is nothing here to
                // compose or edit first.
                // (id `slashPalette`, not `palette`: every Item already has a
                // `palette` property, and an id that shadows one resolves to the
                // property inside any nested Control.)
                Rectangle {
                    id: slashPalette
                    Layout.fillWidth: true
                    // Bound, not assigned: the draft is what decides, so it
                    // re-evaluates on every keystroke without a change handler.
                    property var rows: chatPage.paletteHidden ? [] : Slash.suggest(input.text)
                    visible: rows.length > 0
                    implicitHeight: visible ? paletteCol.implicitHeight + 8 : 0
                    radius: 16
                    color: Theme.surfaceLow
                    border.color: Theme.outlineVar
                    border.width: 1

                    ColumnLayout {
                        id: paletteCol
                        anchors.fill: parent
                        anchors.margins: 4
                        spacing: 0

                        Repeater {
                            model: slashPalette.rows
                            // The row is named so the labels inside contentItem can
                            // reach its `modelData`: a required property lives on the
                            // delegate ROOT, and a nested object's scope chain is
                            // itself + the component root, not everything in between.
                            ItemDelegate {
                                id: slashRow
                                required property var modelData
                                Layout.fillWidth: true
                                // Running a command should leave the caret where the
                                // user left it, so the row never takes focus off the
                                // draft (chat.js binds mousedown for the same reason).
                                focusPolicy: Qt.NoFocus
                                onClicked: chatPage.runCommand(slashRow.modelData.name)
                                contentItem: RowLayout {
                                    spacing: 10
                                    Label {
                                        text: slashRow.modelData.name
                                        color: Theme.surfaceFg
                                        font.pointSize: Theme.baseSize
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: I18n.t(slashRow.modelData.desc)
                                        color: Theme.surfaceVarFg
                                        font.pointSize: Theme.baseSize - 2
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }

                // --- the composer pill ---------------------------------------
                //
                // ONE rounded surface holding the draft on top and a control row
                // (model picker · send) below it, the same shape the web composer
                // and Android's have. The pill owns the outline, so the text area
                // inside it is borderless.
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: pillCol.implicitHeight + 12
                    radius: 24
                    color: Theme.surfaceLow
                    border.color: input.activeFocus ? Theme.primary : Theme.outlineVar
                    border.width: 1

                    ColumnLayout {
                        id: pillCol
                        anchors.fill: parent
                        anchors.margins: 6
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 2

                        ScrollView {
                            Layout.fillWidth: true
                            Layout.maximumHeight: 140
                            Layout.preferredHeight: Math.min(input.implicitHeight, 140)

                            TextArea {
                                id: input
                                placeholderText: I18n.t("messageHint")
                                wrapMode: TextArea.Wrap
                                // The pill draws the outline; this is just text.
                                background: null
                                // Any keystroke undoes an Escape dismissal, so the
                                // palette comes back as soon as the draft moves on.
                                onTextChanged: chatPage.paletteHidden = false
                                // Enter sends; Shift+Enter inserts a newline (chat.js
                                // keydown). Escape closes the palette without
                                // touching the draft.
                                Keys.onPressed: function (event) {
                                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                            && !(event.modifiers & Qt.ShiftModifier)) {
                                        event.accepted = true;
                                        chatPage.submit();
                                    } else if (event.key === Qt.Key_Escape && slashPalette.visible) {
                                        event.accepted = true;
                                        chatPage.paletteHidden = true;
                                    }
                                }
                            }
                        }

                        RowLayout {
                            id: composerRow
                            Layout.fillWidth: true
                            spacing: 8

                            // Balances the send button so the picker sits truly
                            // centered -- the slot Android fills with its paperclip
                            // and this client has nothing to put in (no attachments).
                            Item { Layout.preferredWidth: 40; Layout.preferredHeight: 1 }

                            // Provider / model picker, centered between the two 40px
                            // slots. Restores the cached selection and persists
                            // changes through app.setProvider.
                            ComboBox {
                                id: providerCombo
                                Layout.alignment: Qt.AlignHCenter
                                Layout.fillWidth: true
                                // Grow to fit the widest model name (capped) instead of
                                // eliding it to "gemini-2.5-fla"; +60 leaves room for
                                // the padding and the arrow.
                                Layout.maximumWidth: Math.min(providerMetrics.width + 60, 380)
                                // Server + downloaded on-device providers (from C++), plus a
                                // trailing "Manage on-device AI" entry that opens the manager —
                                // mirrors the Electron/web picker. The label is translated here
                                // so it follows a language switch (I18n.language is a dep).
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
                                    // "Manage on-device AI" isn't a real selection: revert
                                    // to the current provider and open the manager.
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

                            // The composer's primary action: a circular navy button,
                            // as on Android. It carries the stop control too -- this
                            // client can abort a stream, and the button the reply
                            // came from is where a reader looks to end it.
                            Rectangle {
                                id: sendButton
                                Layout.preferredWidth: 40
                                Layout.preferredHeight: 40
                                radius: 20
                                readonly property bool canSend: !app.streaming && input.text.trim().length > 0
                                color: (app.streaming || canSend)
                                       ? Theme.primary
                                       : Qt.rgba(Theme.primary.r, Theme.primary.g, Theme.primary.b, 0.4)

                                Label {
                                    anchors.centerIn: parent
                                    text: app.streaming ? "■" : "↑"
                                    color: Theme.primaryFg
                                    font.pointSize: Theme.baseSize + 2
                                    font.bold: true
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    enabled: app.streaming || sendButton.canSend
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: app.streaming ? app.stopStreaming() : chatPage.submit()
                                    ToolTip.visible: containsMouse && app.streaming
                                    ToolTip.text: I18n.t("close")
                                    hoverEnabled: true
                                }
                            }
                        }
                    }
                }

                // "AI can be wrong", under the composer like every other chat client.
                //
                // ONE line is the requirement, not legibility: a caption nobody reads
                // twice should not steal a second line of thread height on every
                // screen. The width available varies with the window, the language
                // (the German string is half again the English one) and the font-size
                // slider, so the size is DERIVED rather than fixed -- HorizontalFit
                // shrinks it to fit, down to a floor, and elides past that. Android
                // hand-rolls the same measure loop; here Text does it.
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    text: I18n.t("aiDisclaimer")
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.surfaceVarFg
                    opacity: 0.7
                    font.pointSize: Theme.baseSize - 2
                    fontSizeMode: Text.HorizontalFit
                    minimumPointSize: 5
                    maximumLineCount: 1
                    wrapMode: Text.NoWrap
                    elide: Text.ElideRight
                }
            }
        }
    }

    // Run a slash command. Each one does exactly what its control in the drawer
    // does -- /new and /incognito call the very same C++ slots those rows call.
    // The typed command itself is never echoed as a user turn: it was a control,
    // not something the user said.
    function runCommand(name) {
        input.clear();
        paletteHidden = false;
        if (name === Slash.NEW) {
            app.newChat();
        } else if (name === Slash.INCOGNITO) {
            app.toggleIncognito();
        } else if (name === Slash.HELP) {
            // The guide ships inside the binary, so an empty read means the
            // resource is missing -- say so rather than do nothing.
            var guide = app.helpDocument();
            app.appendLocalReply(guide ? guide : I18n.t("requestFailed"));
        }
    }

    function submit() {
        var text = input.text.trim();
        if (!text || app.streaming) return;
        // A command the composer answers by itself, matched on the whole draft
        // (so "what does /help do" is still a question for the model).
        var command = Slash.match(text);
        if (command) { runCommand(command); return; }
        app.sendMessage(text);
        input.clear();
    }

    CostDialog { id: costDialog }
}
