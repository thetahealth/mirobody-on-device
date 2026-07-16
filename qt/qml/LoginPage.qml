import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// Email + one-time-code sign-in, mirroring login.js: enter an email, receive a
// code, type the six digits to verify. (The web client's social sign-ins rely
// on browser SDKs/popups and are intentionally omitted from the native client;
// email login covers every account.) A successful verify flips app.loggedIn,
// which makes Main swap this view for the chat.
Item {
    id: page

    property bool sending: false
    property bool verifying: false
    property bool codeStage: false
    property int  cooldown: 0

    function emailValid(v) { return /^[^\s@]+@[^\s@]+$/.test((v || "").trim()); }

    Connections {
        target: app
        function onCodeSent() {
            page.sending = false;
            page.codeStage = true;
            status.text = I18n.t("codeSentTo", emailField.text.trim());
            page.cooldown = 60;
            cooldownTimer.start();
            codeField.forceActiveFocus();
        }
        function onLoginError(message) {
            page.sending = false;
            status.text = message && message.length ? message : I18n.t("sendFailed");
        }
        function onVerifyError(message) {
            page.verifying = false;
            status.text = message && message.length ? message : I18n.t("verifyFailed");
            codeField.error = true;
        }
    }

    Timer {
        id: cooldownTimer
        interval: 1000; repeat: true
        onTriggered: { page.cooldown -= 1; if (page.cooldown <= 0) stop(); }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: card.implicitHeight + 48
        ColumnLayout {
            id: card
            width: Math.min(parent.width - 40, 400)
            anchors.horizontalCenter: parent.horizontalCenter
            y: Math.max(24, (page.height - implicitHeight) / 2)
            spacing: 14

            Label {
                text: I18n.t("loginTitle")
                font.pointSize: Theme.baseSize + 6
                font.bold: true
                color: Theme.surfaceFg
            }
            Label {
                text: I18n.t("loginSubtitle")
                color: Theme.surfaceVarFg
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            TextField {
                id: emailField
                Layout.fillWidth: true
                placeholderText: "you@example.com"
                inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                onAccepted: if (sendBtn.enabled) sendBtn.clicked()
            }

            // Six-digit code entry, shown after a code is sent.
            TextField {
                id: codeField
                property bool error: false
                visible: page.codeStage
                Layout.fillWidth: true
                placeholderText: "······"
                horizontalAlignment: TextInput.AlignHCenter
                font.pointSize: Theme.baseSize + 6
                font.letterSpacing: 8
                maximumLength: 6
                inputMethodHints: Qt.ImhDigitsOnly
                validator: RegularExpressionValidator { regularExpression: /[0-9]{0,6}/ }
                color: error ? Theme.error : Theme.surfaceFg
                onTextChanged: {
                    error = false;
                    if (text.length === 6 && !page.verifying) verifyBtn.clicked();
                }
            }

            Button {
                id: sendBtn
                Layout.fillWidth: true
                enabled: !page.sending && page.cooldown === 0 && page.emailValid(emailField.text)
                text: page.cooldown > 0 ? I18n.t("resendIn", page.cooldown)
                                        : (page.codeStage ? I18n.t("resendCode") : I18n.t("sendCode"))
                onClicked: {
                    var email = emailField.text.trim();
                    if (!email) { status.text = I18n.t("emailRequired"); return; }
                    page.sending = true;
                    status.text = I18n.t("sendingCode");
                    app.sendCode(email);
                }
            }

            Button {
                id: verifyBtn
                Layout.fillWidth: true
                visible: page.codeStage
                highlighted: true
                enabled: !page.verifying && codeField.text.trim().length === 6
                text: I18n.t("verifyButton")
                onClicked: {
                    var code = codeField.text.trim();
                    if (!code) { status.text = I18n.t("enterCode"); return; }
                    page.verifying = true;
                    status.text = I18n.t("verifying");
                    app.verifyCode(emailField.text.trim(), code);
                }
            }

            Label {
                id: status
                Layout.fillWidth: true
                color: Theme.surfaceVarFg
                wrapMode: Text.WordWrap
                font.pointSize: Theme.baseSize - 1
            }
        }
    }
}
