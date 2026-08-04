import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mirobody

// Email + one-time-code sign-in, mirroring login.js: enter an email, receive a
// code, type the six digits to verify. (The web client's social sign-ins rely
// on browser SDKs/popups and are intentionally omitted from the native client;
// email login covers every account.) A successful verify flips app.loggedIn,
// which makes Main swap this view for the chat.
//
// login.js's staircase, each step unlocking the next: a valid-looking address
// unlocks Send code; a successful send unlocks the code field and starts the resend
// cooldown; six digits unlock Sign in, and also submit on their own.
Item {
    id: page

    property bool sending: false
    property bool verifying: false
    property int  cooldown: 0

    // Lowercased address the last code went to, "" until one is sent. Compared against
    // the field rather than kept as a plain flag, so editing the address re-locks the
    // code field until a code goes to THAT one (and a change back unlocks it again).
    property string codeSentTo: ""

    // At least *@*.* -- stricter than the server's lenient normalize_email (which
    // would take a single-label domain like "user@demo"), so a typo dies here instead
    // of costing a sent code.
    function emailValid(v) { return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test((v || "").trim()); }

    readonly property bool codeSent:
        codeSentTo.length > 0 && codeSentTo === emailField.text.trim().toLowerCase()

    Connections {
        target: app
        function onCodeSent() {
            page.sending = false;
            // A new code means a clean field: drop whatever was typed for the previous
            // one, then unlock it (codeSentTo) and hand it the caret.
            page.codeSentTo = emailField.text.trim().toLowerCase();
            codeField.clear();
            codeField.error = false;
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

            // Six-digit code entry -- always on the card, but inert until a code has
            // actually gone to the address above it (login.js locks it the same way
            // rather than hiding it, so the whole form is visible from the start).
            TextField {
                id: codeField
                property bool error: false
                enabled: page.codeSent
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
                    // Six digits submit on their own, through the same gate the button
                    // uses -- so this can never fire on a code Sign in would refuse.
                    if (verifyBtn.enabled) verifyBtn.clicked();
                }
                onAccepted: if (verifyBtn.enabled) verifyBtn.clicked()
            }

            Button {
                id: sendBtn
                Layout.fillWidth: true
                enabled: !page.sending && page.cooldown === 0 && page.emailValid(emailField.text)
                text: page.cooldown > 0 ? I18n.t("resendIn", page.cooldown)
                                        : (page.codeSentTo.length > 0 ? I18n.t("resendCode")
                                                                     : I18n.t("sendCode"))
                onClicked: {
                    var email = emailField.text.trim();
                    if (!page.emailValid(email)) { status.text = I18n.t("emailRequired"); return; }
                    page.sending = true;
                    status.text = I18n.t("sendingCode");
                    app.sendCode(email);
                }
            }

            // Sign in needs the whole staircase: a valid address, a code sent to it,
            // and all six digits.
            Button {
                id: verifyBtn
                Layout.fillWidth: true
                highlighted: true
                enabled: !page.verifying && page.codeSent && codeField.text.trim().length === 6
                // "Sign in", as login.js labels the primary button -- it stands on the
                // card from the start now, not only once a code has been sent, and
                // "Verify" read as a step rather than the action that ends the flow.
                text: I18n.t("loginTitle")
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
