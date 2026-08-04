import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// The left navigation drawer -- the app's ONLY menu (history.js openHistory).
//   top    : a "New chat" / "Incognito" button row (pinned)
//   middle : the scrolling history list (GET /api/history), each row tap-to-resume
//            (app.openConversation) and deletable (app.deleteHistory)
//   bottom : pinned groups -- Health & data (Connect EHR, Connected devices,
//            Bluetooth devices), app settings (language / font / backend),
//            and the account switcher (current email + caret -> other accounts +
//            Add account) + Sign out (confirmed).
//
// The app-settings group used to be the top-bar gear, and was the sole reason the
// login screen had a right-hand action at all. Folding it in here leaves one menu
// affordance instead of two -- and lets the login screen open this same drawer
// showing just that group, since everything else is session-scoped.
Drawer {
    id: drawer
    edge: I18n.isRtl(app.language) ? Qt.RightEdge : Qt.LeftEdge
    width: Math.min(parent ? parent.width * 0.85 : 360, 360)
    height: parent ? parent.height : 0

    // Health & data actions live as dialogs in Main; the drawer just requests them.
    signal openBle()
    signal openEhr()
    signal openVendors()
    // App settings, likewise owned by Main so the drawer can close before one opens.
    signal openLanguage()
    signal openFont()
    signal openBackend()

    // Signed out (the login screen's drawer) only the app-settings group applies:
    // history, New chat / Incognito, health connections and the account rows are all
    // session-scoped and are left out entirely.
    readonly property bool signedIn: app.loggedIn && !app.addingAccount

    property string status: "loading"   // "loading" | "error" | "list"
    property string errorMessage: ""
    property string pendingDelete: ""
    property var    accounts: []
    property bool   switcherOpen: false

    function reload() {
        status = "loading";
        items.clear();
        switcherOpen = false;
        // Nothing to load signed out, and /api/history would 401 -- the drawer is
        // just the settings group then.
        if (!signedIn) { accounts = []; return; }
        accounts = app.listAccounts();
        app.loadHistory(0, 20);
    }

    // One footer row: label on the leading edge, current value on the trailing edge,
    // so "Language  English" is answerable without opening the dialog.
    component NavRow: ItemDelegate {
        id: navRow
        property string rowLabel: ""
        property string rowHint: ""
        contentItem: RowLayout {
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: navRow.rowLabel
                color: Theme.surfaceFg
                elide: Text.ElideRight
            }
            Label {
                visible: navRow.rowHint.length > 0
                text: navRow.rowHint
                color: Theme.surfaceVarFg
                font.pointSize: Theme.baseSize - 2
                elide: Text.ElideRight
            }
        }
    }

    function pad(n) { return (n < 10 ? "0" : "") + n; }
    // The backend sends `timestamp` as epoch milliseconds (UTC); a legacy ISO-8601
    // string is also tolerated. Format in the device's local zone. Mirrors
    // formatTimestamp in htdoc/src/format.js and HistoryScreen.kt.
    function formatTimestamp(raw) {
        if (!raw) return "";
        var d;
        if (typeof raw === "number" || /^\d+$/.test(String(raw).trim())) {
            d = new Date(Number(raw));                   // unix milliseconds
        } else {
            var s = String(raw).trim().replace(" ", "T");
            s = s.replace(/([+\-]\d\d)(\d\d)$/, "$1:$2");
            s = s.replace(/([+\-]\d\d)$/, "$1:00");
            if (!/[zZ]$|[+\-]\d\d:\d\d$/.test(s)) s += "Z";
            d = new Date(s);
        }
        if (isNaN(d.getTime())) return String(raw);
        return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate())
             + " " + pad(d.getHours()) + ":" + pad(d.getMinutes());
    }

    function acctLabel(a) {
        if (!a) return "";
        return a.email && a.email.length ? a.email : ("#" + String(a.sub).slice(0, 6));
    }
    function currentAccount() {
        for (var i = 0; i < accounts.length; ++i)
            if (accounts[i].current) return accounts[i];
        return accounts.length ? accounts[0] : null;
    }

    Connections {
        target: app
        function onHistoryLoaded(summaries) {
            items.clear();
            for (var i = 0; i < summaries.length; ++i) {
                var it = summaries[i];
                items.append({
                    sessionId: it.session_id || "",
                    summary:   it.summary || it.session_id || I18n.t("historyUntitled"),
                    timestamp: it.timestamp || "",
                    owned:     it.owned === undefined ? true : it.owned,
                    sharedBy:  it.shared_by || "",
                    sharedWith: it.shared_with_count || 0
                });
            }
            drawer.status = "list";
        }
        function onHistoryError(message) {
            drawer.errorMessage = message && message.length ? message : I18n.t("historyLoadFailed");
            drawer.status = "error";
        }
        function onAccountsChanged() { drawer.accounts = app.listAccounts(); }
    }

    ListModel { id: items }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- Header --------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            Layout.leftMargin: 8
            spacing: 4
            ToolButton { text: "←"; font.pointSize: Theme.baseSize + 4; onClicked: drawer.close() }
            Label {
                text: I18n.t("menuTitle")
                font.pointSize: Theme.baseSize + 1
                color: Theme.surfaceFg
            }
        }

        // --- Top row: New chat + Incognito toggle (pinned) -----------------
        // Signed out there is no thread to start or hide. An invisible item is
        // skipped by ColumnLayout, so the group below simply moves up.
        RowLayout {
            visible: drawer.signedIn
            Layout.fillWidth: true
            Layout.margins: 12
            spacing: 8
            Button {
                Layout.fillWidth: true
                text: I18n.t("newChat")
                flat: true
                onClicked: { app.newChat(); drawer.close(); }
            }
            Button {
                Layout.fillWidth: true
                text: I18n.t("incognitoMode")
                flat: true
                checkable: true
                checked: app.incognito
                palette.buttonText: app.incognito ? Theme.primary : Theme.surfaceFg
                ToolTip.visible: hovered
                ToolTip.text: app.incognito ? I18n.t("incognitoStop") : I18n.t("incognitoStart")
                onClicked: { app.toggleIncognito(); drawer.close(); }
            }
        }

        // --- Middle: history list (the only scrolling area) ----------------
        // This is the one item with fillHeight, so hiding it signed out is also what
        // stops the settings group being pushed to the bottom of an empty drawer:
        // with no stretching item left, everything sits under the header.
        Item {
            visible: drawer.signedIn
            Layout.fillWidth: true
            Layout.fillHeight: true

            BusyIndicator {
                anchors.centerIn: parent
                running: drawer.status === "loading"
                visible: running
            }

            ColumnLayout {
                anchors.centerIn: parent
                visible: drawer.status === "error"
                spacing: 10
                Label { text: drawer.errorMessage; color: Theme.error }
                Button { text: I18n.t("retry"); onClicked: drawer.reload() }
            }

            Label {
                anchors.centerIn: parent
                visible: drawer.status === "list" && items.count === 0
                text: I18n.t("historyEmpty")
                color: Theme.surfaceVarFg
            }

            ListView {
                anchors.fill: parent
                clip: true
                visible: drawer.status === "list" && items.count > 0
                model: items
                delegate: ItemDelegate {
                    width: ListView.view.width
                    required property string sessionId
                    required property string summary
                    required property string timestamp
                    required property bool   owned
                    required property string sharedBy
                    required property int    sharedWith
                    required property int index
                    height: rowCol.implicitHeight + 20
                    // Tap the row to resume the conversation into the chat view.
                    onClicked: { app.openConversation(sessionId); drawer.close(); }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 16
                        anchors.rightMargin: 4
                        ColumnLayout {
                            id: rowCol
                            Layout.fillWidth: true
                            spacing: 4
                            Label {
                                Layout.fillWidth: true
                                text: summary
                                color: Theme.surfaceFg
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            // Sharing badges: "Shared by <owner>" on a thread shared
                            // to me; "Shared with N" on one of mine that I've shared.
                            Label {
                                visible: !owned && sharedBy.length > 0
                                text: I18n.t("sharedByBadge", sharedBy)
                                color: Theme.primary
                                font.pointSize: Theme.baseSize - 3
                            }
                            Label {
                                visible: owned && sharedWith > 0
                                text: I18n.t("sharedWithBadge", sharedWith)
                                color: Theme.surfaceVarFg
                                font.pointSize: Theme.baseSize - 3
                            }
                            Label {
                                visible: timestamp.length > 0
                                text: drawer.formatTimestamp(timestamp)
                                color: Theme.surfaceVarFg
                                opacity: 0.7
                                font.pointSize: Theme.baseSize - 3
                            }
                        }
                        ToolButton {
                            text: "🗑"
                            onClicked: {
                                drawer.pendingDelete = sessionId;
                                confirm.message = I18n.t("historyDeleteMessage");
                                confirm.title = I18n.t("historyDeleteTitle");
                                confirm.confirmText = I18n.t("delete");
                                confirm.pendingAction = "delete";
                                confirm.open();
                            }
                        }
                    }
                }
            }
        }

        // --- Footer: Health & data + app settings + account (all pinned) ---
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.outlineVar }

            // Health & data group label.
            Label {
                visible: drawer.signedIn
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.leftMargin: 20
                text: I18n.t("healthData")
                color: Theme.surfaceVarFg
                font.pointSize: Theme.baseSize - 2
                font.bold: true
            }

            ItemDelegate {
                visible: drawer.signedIn
                Layout.fillWidth: true
                text: I18n.t("ehrConnect")
                onClicked: { drawer.close(); drawer.openEhr(); }
            }
            ItemDelegate {
                visible: drawer.signedIn
                Layout.fillWidth: true
                text: I18n.t("vendorManageTitle")
                onClicked: { drawer.close(); drawer.openVendors(); }
            }
            // The direct BLE GATT sensor connect, moved here from the settings gear.
            ItemDelegate {
                visible: drawer.signedIn
                Layout.fillWidth: true
                text: I18n.t("bluetoothDevices")
                onClicked: { drawer.close(); drawer.openBle(); }
            }

            Rectangle {
                visible: drawer.signedIn
                Layout.fillWidth: true; Layout.topMargin: 4
                Layout.preferredHeight: 1; color: Theme.outlineVar; opacity: 0.5
            }

            // App settings -- what the gear used to hold, and the only group the login
            // screen shows. Each row closes the drawer first, then opens its dialog.
            NavRow {
                Layout.fillWidth: true
                rowLabel: I18n.t("language")
                rowHint: I18n.languageLabel(app.language)
                onClicked: { drawer.close(); drawer.openLanguage(); }
            }
            NavRow {
                Layout.fillWidth: true
                rowLabel: I18n.t("fontSize")
                rowHint: I18n.fontTierLabel(app.fontOffset)
                onClicked: { drawer.close(); drawer.openFont(); }
            }
            NavRow {
                Layout.fillWidth: true
                rowLabel: I18n.t("backend")
                rowHint: app.baseUrl
                onClicked: { drawer.close(); drawer.openBackend(); }
            }

            Rectangle {
                visible: drawer.signedIn
                Layout.fillWidth: true; Layout.topMargin: 4
                Layout.preferredHeight: 1; color: Theme.outlineVar; opacity: 0.5
            }

            // Account switcher: the current email + caret expands the other stored
            // accounts (tap to switch) plus "Add account".
            ItemDelegate {
                visible: drawer.signedIn
                Layout.fillWidth: true
                onClicked: drawer.switcherOpen = !drawer.switcherOpen
                contentItem: RowLayout {
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: drawer.acctLabel(drawer.currentAccount())
                        color: Theme.surfaceVarFg
                        font.pointSize: Theme.baseSize - 1
                        elide: Text.ElideRight
                    }
                    Label {
                        text: drawer.switcherOpen ? "▴" : "▾"
                        color: Theme.surfaceVarFg
                    }
                }
            }

            // Expanded switcher: other accounts + Add account.
            Column {
                Layout.fillWidth: true
                visible: drawer.signedIn && drawer.switcherOpen
                Repeater {
                    model: drawer.accounts
                    delegate: ItemDelegate {
                        required property var modelData
                        width: parent ? parent.width : 0
                        visible: !modelData.current
                        height: visible ? implicitHeight : 0
                        text: drawer.acctLabel(modelData)
                        onClicked: { drawer.close(); app.switchAccount(modelData.sub); }
                    }
                }
                ItemDelegate {
                    width: parent ? parent.width : 0
                    text: "＋  " + I18n.t("addAccount")
                    onClicked: { drawer.close(); app.addAccount(); }
                }
            }

            // Sign out the current account (confirmed). app.signOut then falls back
            // to another stored account, or the login screen when none remain.
            ItemDelegate {
                visible: drawer.signedIn
                Layout.fillWidth: true
                Layout.bottomMargin: 6
                contentItem: Label {
                    text: I18n.t("signOut")
                    color: Theme.error
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    confirm.title = I18n.t("signOut");
                    confirm.message = I18n.t("signOutConfirm");
                    confirm.confirmText = I18n.t("signOut");
                    confirm.pendingAction = "signOut";
                    confirm.open();
                }
            }
        }
    }

    // One confirm dialog shared by the delete and sign-out flows.
    ConfirmDialog {
        id: confirm
        property string pendingAction: ""
        confirmText: I18n.t("delete")
        onConfirmed: {
            if (pendingAction === "signOut") {
                drawer.close();
                app.signOut();
                return;
            }
            var id = drawer.pendingDelete;
            for (var i = 0; i < items.count; ++i) {
                if (items.get(i).sessionId === id) { items.remove(i); break; }
            }
            app.deleteHistory(id);
        }
    }
}
