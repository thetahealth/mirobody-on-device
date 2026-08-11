import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Mirobody

// On-device model manager, the Qt analogue of Android's OnDeviceModelDialog: the
// built-in catalog on top, each row carrying its own download / progress / delete,
// then the models the user brought themselves (a file already on disk, or any other
// GGUF by URL — the desktop's version of Android's "Import file…").
//
// Also where a model is CHOSEN: clicking a downloaded model's name picks it and closes
// the dialog. It still appears in the composer's provider picker — that list is the one
// place every backend, server or local, is comparable — but making the user go back to
// it after downloading here was a step with nothing behind it.
//
// Reached from the ⚙ menu and from the picker's "Manage on-device AI" entry.
Dialog {
    id: dialog
    title: I18n.t("onDeviceAi")
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(parent ? parent.width - 40 : 520, 520)
    // Pin the content to the dialog rather than the other way round. A wrapping Text and
    // a Label with a long file name both report their *unwrapped* width as implicit, so
    // without this the content item sizes itself to ~1600px and spills out of a 420px
    // dialog — the frame stays put and the buttons walk off the right edge.
    contentWidth: availableWidth
    standardButtons: Dialog.Close

    // Deleting is destructive — a downloaded file is removed from disk; a file the user
    // pointed us at is only forgotten — so it goes through a confirm. On the root
    // because a delegate cannot see an inner id.
    function confirmRemove(id, name, remote) {
        deleteConfirm.pendingId = id;
        deleteConfirm.message = I18n.t(remote ? "deleteModelConfirm" : "forgetModelConfirm", name);
        deleteConfirm.open();
    }

    // One model, catalog or user-added. The two lists differ only in what ✕ means
    // (a catalog row survives its file being deleted and offers the download again),
    // which the `builtin` flag already carries, so both Repeaters share this.
    Component {
        id: modelRow
        ColumnLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: 2
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                ColumnLayout {
                    Layout.fillWidth: true
                    // Without a floor of 0 the labels' natural width becomes the row's
                    // minimum, and a long file name pushes the buttons off the dialog
                    // instead of eliding.
                    Layout.minimumWidth: 0
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        text: modelData.name
                        color: Theme.surfaceFg
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        font.pointSize: Theme.baseSize - 3
                        color: Theme.surfaceVarFg
                        elide: Text.ElideRight
                        // "~2.9 GB · 6 GB+ RAM" for a catalog model; a user entry has no
                        // RAM guidance to give, so it names the file instead.
                        text: {
                            var size = modelData.size ? "~" + modelData.size : "";
                            var tail = modelData.ram ? modelData.ram + " RAM" : modelData.file;
                            return size && tail ? size + " · " + tail : (size || tail || "");
                        }
                    }
                    // The name IS the affordance. Inert until the file is there, so a
                    // click on a model still downloading cannot select something absent.
                    // Handlers rather than a MouseArea: a layout would lay an Item out.
                    TapHandler {
                        enabled: modelData.status === "ready"
                        onTapped: {
                            app.selectOnDeviceModel(modelData.id);
                            dialog.close();
                        }
                    }
                    HoverHandler {
                        enabled: modelData.status === "ready"
                        cursorShape: Qt.PointingHandCursor
                    }
                }
                Button {
                    text: I18n.t("download")
                    flat: true
                    visible: modelData.remote && modelData.status === "absent"
                    onClicked: app.onDeviceModel.download(modelData.id)
                }
                Button {
                    text: I18n.t("retry")
                    flat: true
                    visible: modelData.status === "failed"
                    onClicked: app.onDeviceModel.download(modelData.id)
                }
                Button {
                    text: I18n.t("cancel")
                    flat: true
                    visible: modelData.status === "downloading"
                    onClicked: app.onDeviceModel.cancel()
                }
                Button {
                    id: delBtn
                    flat: true
                    visible: modelData.status !== "downloading"
                             && (!modelData.builtin || modelData.status === "ready")
                    text: modelData.builtin || modelData.remote ? I18n.t("deleteModel")
                                                                : I18n.t("forgetModel")
                    // Red label so the destructive action reads as such (Fusion buttons
                    // don't tint by role, so override the content).
                    contentItem: Text {
                        text: delBtn.text
                        font: delBtn.font
                        color: Theme.error
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                    onClicked: dialog.confirmRemove(modelData.id, modelData.name, modelData.remote)
                }
            }

            // A download in flight: a real bar plus the byte counts, so a multi-GB wait
            // reads as progress rather than a stall. The bar is indeterminate until the
            // server reports a Content-Length.
            ProgressBar {
                Layout.fillWidth: true
                visible: modelData.status === "downloading"
                indeterminate: !modelData.total
                value: modelData.progress
            }
            Label {
                Layout.fillWidth: true
                font.pointSize: Theme.baseSize - 3
                wrapMode: Text.WordWrap
                visible: text.length > 0
                color: modelData.status === "failed" ? Theme.error
                     : modelData.status === "ready"  ? Theme.primary
                                                     : Theme.surfaceVarFg
                text: {
                    switch (modelData.status) {
                    case "ready":       return I18n.t("modelReady");
                    case "downloading": return modelData.total
                            ? modelData.downloaded + " / " + modelData.total
                            : I18n.t("modelDownloading");
                    case "failed":      return modelData.error;
                    default:            return modelData.remote ? "" : I18n.t("modelFileMissing");
                    }
                }
            }
        }
    }

    ColumnLayout {
        id: body
        // Driven off the dialog, not the other way round: see contentWidth above.
        width: dialog.availableWidth
        spacing: 10

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.surfaceVarFg
            font.pointSize: Theme.baseSize - 1
            text: I18n.t("onDeviceIntro")
        }

        // Catalog + the user's own, in one scroller: together they are the list, and
        // splitting the scrolling would make the second half easy to miss.
        ScrollView {
            id: modelScroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(modelsColumn.implicitHeight, 320)
            contentWidth: availableWidth   // vertical scrolling only
            clip: true

            ColumnLayout {
                id: modelsColumn
                width: modelScroll.availableWidth
                spacing: 12

                Repeater { model: app.onDeviceModel.catalog; delegate: modelRow }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.outlineVar
                }

                Label {
                    text: I18n.t("yourModels")
                    color: Theme.surfaceVarFg
                    font.pointSize: Theme.baseSize - 1
                }
                Repeater { model: app.onDeviceModel.imported; delegate: modelRow }
            }
        }

        // Add any other GGUF by URL: name + URL. llama.cpp is model-agnostic, so the
        // catalog above is a starting point rather than the limit.
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            TextField {
                id: addName
                Layout.preferredWidth: 96
                placeholderText: I18n.t("modelName")
            }
            TextField {
                id: addUrl
                Layout.fillWidth: true
                placeholderText: "https://…/model.gguf"
            }
            Button {
                text: I18n.t("add")
                enabled: addUrl.text.trim().length > 0
                onClicked: { app.onDeviceModel.addRemote(addName.text, addUrl.text); addName.clear(); addUrl.clear(); }
            }
        }

        Button {
            Layout.fillWidth: true
            text: I18n.t("addLocalFile")
            onClicked: modelFileDialog.open()
        }
    }

    // Register an existing GGUF as a local model (no copy is made).
    FileDialog {
        id: modelFileDialog
        title: I18n.t("selectGgufModel")
        nameFilters: ["GGUF models (*.gguf)", "All files (*)"]
        onAccepted: app.onDeviceModel.addLocal("", selectedFile)
    }

    // Destructive-action confirm (delete the downloaded file / forget the user's own).
    ConfirmDialog {
        id: deleteConfirm
        property string pendingId: ""
        title: I18n.t("deleteModel")
        confirmText: I18n.t("delete")
        onConfirmed: if (pendingId.length) app.onDeviceModel.remove(pendingId)
    }
}
