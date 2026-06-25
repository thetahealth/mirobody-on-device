import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// The history drawer (history.js): a side panel listing past server-side
// sessions (GET /api/history via app.loadHistory), each deletable
// (app.deleteHistory). List + delete only, no resume -- as in the web client.
Drawer {
    id: drawer
    edge: I18n.isRtl(app.language) ? Qt.RightEdge : Qt.LeftEdge
    width: Math.min(parent ? parent.width * 0.85 : 360, 360)
    height: parent ? parent.height : 0

    property string status: "loading"   // "loading" | "error" | "list"
    property string errorMessage: ""
    property string pendingDelete: ""

    function reload() {
        status = "loading";
        items.clear();
        app.loadHistory(0, 20);
    }

    function pad(n) { return (n < 10 ? "0" : "") + n; }
    function formatTimestamp(raw) {
        if (!raw) return "";
        var s = String(raw).trim().replace(" ", "T");
        s = s.replace(/([+\-]\d\d)(\d\d)$/, "$1:$2");
        s = s.replace(/([+\-]\d\d)$/, "$1:00");
        if (!/[zZ]$|[+\-]\d\d:\d\d$/.test(s)) s += "Z";
        var d = new Date(s);
        if (isNaN(d.getTime())) return raw;
        return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate())
             + " " + pad(d.getHours()) + ":" + pad(d.getMinutes());
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
                    timestamp: it.timestamp || ""
                });
            }
            drawer.status = "list";
        }
        function onHistoryError(message) {
            drawer.errorMessage = message && message.length ? message : I18n.t("historyLoadFailed");
            drawer.status = "error";
        }
    }

    ListModel { id: items }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            Layout.leftMargin: 8
            spacing: 4
            ToolButton { text: "←"; font.pointSize: Theme.baseSize + 4; onClicked: drawer.close() }
            Label {
                text: I18n.t("historyTitle")
                font.pointSize: Theme.baseSize + 1
                color: Theme.onSurface
            }
        }

        // Body: loading / error / empty / list.
        Item {
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
                color: Theme.onSurfaceVar
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
                    required property int index
                    height: rowCol.implicitHeight + 20

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
                                color: Theme.onSurface
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            Label {
                                visible: timestamp.length > 0
                                text: drawer.formatTimestamp(timestamp)
                                color: Theme.onSurfaceVar
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
                                confirm.open();
                            }
                        }
                    }
                }
            }
        }
    }

    ConfirmDialog {
        id: confirm
        confirmText: I18n.t("delete")
        onConfirmed: {
            var id = drawer.pendingDelete;
            for (var i = 0; i < items.count; ++i) {
                if (items.get(i).sessionId === id) { items.remove(i); break; }
            }
            app.deleteHistory(id);
        }
    }
}
