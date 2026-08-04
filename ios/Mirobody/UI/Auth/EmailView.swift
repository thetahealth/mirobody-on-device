import SwiftUI

/// Email + one-time-code sign-in on ONE screen — mirrors `ui/auth/EmailScreen.kt` and
/// the web client's login card (`htdoc/src/login.js`). There is no separate verify
/// screen: the code sits under the address it was sent to.
///
/// The form is login.js's staircase, each step unlocking the next: a valid-looking
/// address unlocks Send code; a successful send unlocks the code boxes and starts the
/// 60s resend cooldown (editing the address re-locks them until a code is sent to that
/// one); six digits unlock Sign in, and also submit on their own. One status line under
/// the buttons narrates all of it, federated sign-in included.
struct EmailView: View {
    @EnvironmentObject private var container: AppContainer
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    @StateObject private var vm: EmailViewModel
    private let onSignedIn: () -> Void

    @FocusState private var codeFocused: Bool

    init(container: AppContainer, onSignedIn: @escaping () -> Void) {
        self.onSignedIn = onSignedIn
        _vm = StateObject(wrappedValue: EmailViewModel(
            repo: container.authRepository,
            googleRepo: container.googleAuthRepository,
            appleRepo: container.appleAuthRepository,
            wechatRepo: container.wechatAuthRepository,
            githubRepo: container.githubAuthRepository,
            xRepo: container.xAuthRepository,
            initialEmail: container.settings.lastEmail
        ))
    }

    var body: some View {
        ZStack(alignment: .top) {
            colors.background.ignoresSafeArea()
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    Spacer().frame(height: 24)
                    LText("email_title").mbFont(.headlineMedium).foregroundColor(colors.onSurface)
                    Spacer().frame(height: 8)
                    LText("email_subtitle").mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)
                    Spacer().frame(height: 28)

                    emailField
                    Spacer().frame(height: 12)
                    sendCodeButton
                    Spacer().frame(height: 20)
                    codeBoxes
                    Spacer().frame(height: 20)
                    signInButton
                    Spacer().frame(height: 10)
                    statusLine

                    socialBlock
                    // The card is taller than the old two-screen split (address, code
                    // and Sign in all sit here now), so it scrolls -- which is also what
                    // keeps every control reachable with the keyboard up.
                    Spacer().frame(height: 32)
                }
                .padding(.horizontal, 24)
                .frame(maxWidth: contentMaxWidth)
                .frame(maxWidth: .infinity)
            }
        }
        .navigationBarTitleDisplayMode(.inline)
        // A code is on its way to the address in the field: hand the caret to the boxes.
        // Keyed on the address, so a send to a NEW one moves it again.
        .onChange(of: vm.codeSentTo) { sentTo in
            if !sentTo.isEmpty { codeFocused = true }
        }
        // Six digits submit on their own, through the same gate the Sign in button uses,
        // so this can never fire on a code the button would refuse. A rejected code stays
        // in the boxes unchanged, which is what stops it from re-firing.
        .onChange(of: vm.code) { _ in
            if vm.canVerify { vm.verify(onSignedIn: onSignedIn) }
        }
    }

    /// The address. Valid shape unlocks Send code, and the keyboard's Send key sends too
    /// (login.js binds Enter here to the same button). No error tint — the status line
    /// carries every message, as on the web.
    private var emailField: some View {
        TextField("", text: Binding(get: { vm.email }, set: vm.onEmailChange))
            .textFieldStyle(.plain)
            .keyboardType(.emailAddress)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .submitLabel(.send)
            .onSubmit { if vm.canSendCode { vm.sendCode() } }
            .mbFont(.bodyLarge)
            .foregroundColor(colors.onSurface)
            .padding(14)
            .overlay(RoundedRectangle(cornerRadius: 10)
                .stroke(colors.outlineVariant, lineWidth: 1))
            .overlay(alignment: .topLeading) {
                if vm.email.isEmpty {
                    LText("email_label").mbFont(.bodyLarge)
                        .foregroundColor(colors.onSurfaceVariant).padding(14).allowsHitTesting(false)
                }
            }
    }

    /// Outlined secondary action — Sign in below is the screen's primary button now.
    /// A request in flight and the cooldown after it each own the label; once a code has
    /// been sent it reads "Resend code".
    private var sendCodeButton: some View {
        Button(action: { vm.sendCode() }) {
            Group {
                if vm.sending {
                    ProgressView().tint(colors.primary)
                } else if vm.cooldownSeconds > 0 {
                    LText("email_resend_in", vm.cooldownSeconds).mbFont(.labelLarge)
                } else if !vm.codeSentTo.isEmpty {
                    LText("verify_resend_code").mbFont(.labelLarge)
                } else {
                    LText("email_send_code").mbFont(.labelLarge)
                }
            }
            .frame(maxWidth: .infinity, minHeight: 52)
            .foregroundColor(colors.primary)
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(colors.outline, lineWidth: 1))
        }
        .disabled(!vm.canSendCode)
        .opacity(vm.canSendCode ? 1 : 0.5)
    }

    /// Sign in needs the whole staircase: a valid address, a code sent to it, and all six
    /// digits.
    private var signInButton: some View {
        Button(action: { vm.verify(onSignedIn: onSignedIn) }) {
            Group {
                if vm.verifying { ProgressView().tint(colors.onPrimary) }
                else { LText("email_title").mbFont(.labelLarge) }
            }
            .frame(maxWidth: .infinity, minHeight: 52)
            .foregroundColor(colors.onPrimary)
            .background(colors.primary)
            .clipShape(RoundedRectangle(cornerRadius: 10))
        }
        .disabled(!vm.canVerify)
        .opacity(vm.canVerify ? 1 : 0.5)
    }

    /// The one status line every flow writes to (login.js's `status`): progress and
    /// confirmations in the muted ink, failures in the error ink — the server's own
    /// message when it sent one, else the flow's fallback.
    @ViewBuilder
    private var statusLine: some View {
        switch vm.status {
        case .idle:
            EmptyView()
        case .info(let key, let arg):
            Text(verbatim: arg.map { L(key, lang, $0) } ?? L(key, lang))
                .mbFont(.bodySmall).foregroundColor(colors.onSurfaceVariant)
        case .error(let key, let message):
            Text(verbatim: message ?? L(key, lang))
                .mbFont(.bodySmall).foregroundColor(colors.error)
        }
    }

    /// Six digit slots over an invisible field that owns the keyboard. Inert until a code
    /// has actually gone to the address above them — login.js locks its code field the
    /// same way rather than hiding it, so the whole form is visible from the start.
    private var codeBoxes: some View {
        ZStack {
            HStack(spacing: 8) {
                ForEach(0..<oneTimeCodeLength, id: \.self) { i in
                    let chars = Array(vm.code)
                    let digit = i < chars.count ? String(chars[i]) : ""
                    let isFocusedSlot = vm.codeSent && vm.code.count == i
                    Text(digit)
                        .mbFont(.headlineSmall)
                        .foregroundColor(colors.onSurface)
                        .frame(maxWidth: .infinity)
                        .aspectRatio(0.8, contentMode: .fit)
                        .background(colors.surfaceContainerLow)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                        .overlay(
                            RoundedRectangle(cornerRadius: 8).stroke(
                                vm.codeRejected ? colors.error
                                    : (isFocusedSlot ? colors.primary : colors.outlineVariant),
                                lineWidth: (isFocusedSlot || vm.codeRejected) ? 2 : 1
                            )
                        )
                }
            }
            // Invisible field that owns the keyboard and feeds onCodeChange.
            TextField("", text: Binding(get: { vm.code }, set: vm.onCodeChange))
                .keyboardType(.numberPad)
                .focused($codeFocused)
                .opacity(0.001)
                .disabled(!vm.codeSent)
        }
        .contentShape(Rectangle())
        .opacity(vm.codeSent ? 1 : 0.5)
        .onTapGesture { if vm.codeSent { codeFocused = true } }
    }

    /// The federated providers this build can actually complete, under an OR divider.
    @ViewBuilder
    private var socialBlock: some View {
        if vm.googleAvailable || vm.appleAvailable || vm.wechatAvailable || vm.githubAvailable || vm.xAvailable {
            Spacer().frame(height: 24)
            HStack(spacing: 12) {
                Rectangle().fill(colors.outlineVariant).frame(height: 1)
                LText("auth_or").mbFont(.labelMedium).foregroundColor(colors.onSurfaceVariant)
                Rectangle().fill(colors.outlineVariant).frame(height: 1)
            }
            Spacer().frame(height: 20)
            if vm.googleAvailable {
                socialButton(titleKey: "auth_continue_with_google",
                             loading: vm.googleSigningIn) {
                    vm.signInWithGoogle(onSignedIn: onSignedIn)
                }
            }
            if vm.appleAvailable {
                if vm.googleAvailable { Spacer().frame(height: 12) }
                socialButton(titleKey: "auth_continue_with_apple",
                             loading: vm.appleSigningIn) {
                    vm.signInWithApple(onSignedIn: onSignedIn)
                }
            }
            if vm.wechatAvailable {
                if vm.googleAvailable || vm.appleAvailable { Spacer().frame(height: 12) }
                socialButton(titleKey: "auth_continue_with_wechat",
                             loading: vm.wechatSigningIn) {
                    vm.signInWithWeChat(onSignedIn: onSignedIn)
                }
            }
            if vm.githubAvailable {
                if vm.googleAvailable || vm.appleAvailable || vm.wechatAvailable { Spacer().frame(height: 12) }
                socialButton(titleKey: "auth_continue_with_github",
                             loading: vm.githubSigningIn) {
                    vm.signInWithGithub(onSignedIn: onSignedIn)
                }
            }
            if vm.xAvailable {
                if vm.googleAvailable || vm.appleAvailable || vm.wechatAvailable || vm.githubAvailable { Spacer().frame(height: 12) }
                socialButton(titleKey: "auth_continue_with_x",
                             loading: vm.xSigningIn) {
                    vm.signInWithX(onSignedIn: onSignedIn)
                }
            }
        }
    }

    /// A neutral outlined federated-sign-in button, used for every provider. Disabled
    /// while any sign-in, send or verify is in flight.
    @ViewBuilder
    private func socialButton(
        titleKey: String,
        loading: Bool,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Group {
                if loading {
                    ProgressView().tint(colors.primary)
                } else {
                    LText(titleKey).mbFont(.labelLarge)
                }
            }
            .frame(maxWidth: .infinity, minHeight: 52)
            .foregroundColor(colors.primary)
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(colors.outline, lineWidth: 1))
        }
        .disabled(vm.anyBusy)
    }
}
