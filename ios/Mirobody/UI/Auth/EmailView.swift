import SwiftUI

/// Email sign-in screen — mirrors `ui/auth/EmailScreen.kt`.
struct EmailView: View {
    @EnvironmentObject private var container: AppContainer
    @Environment(\.mbColors) private var colors
    @Environment(\.mbLanguage) private var lang

    @StateObject private var vm: EmailViewModel
    private let onCodeSent: (String) -> Void
    private let onSignedIn: () -> Void

    init(container: AppContainer,
         onCodeSent: @escaping (String) -> Void,
         onSignedIn: @escaping () -> Void) {
        self.onCodeSent = onCodeSent
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
            VStack(alignment: .leading, spacing: 0) {
                Spacer().frame(height: 24)
                LText("email_title").mbFont(.headlineMedium).foregroundColor(colors.onSurface)
                Spacer().frame(height: 8)
                LText("email_subtitle").mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)
                Spacer().frame(height: 28)

                TextField("", text: Binding(get: { vm.email }, set: vm.onEmailChange))
                    .textFieldStyle(.plain)
                    .keyboardType(.emailAddress)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .mbFont(.bodyLarge)
                    .foregroundColor(colors.onSurface)
                    .padding(14)
                    .overlay(RoundedRectangle(cornerRadius: 10)
                        .stroke(vm.error != nil ? colors.error : colors.outlineVariant, lineWidth: 1))
                    .overlay(alignment: .topLeading) {
                        if vm.email.isEmpty {
                            LText("email_label").mbFont(.bodyLarge)
                                .foregroundColor(colors.onSurfaceVariant).padding(14).allowsHitTesting(false)
                        }
                    }
                if let err = vm.error {
                    Spacer().frame(height: 6)
                    Text(err).mbFont(.bodySmall).foregroundColor(colors.error)
                }

                Spacer().frame(height: 20)
                Button(action: { vm.sendCode(onSent: onCodeSent) }) {
                    Group {
                        if vm.sending {
                            ProgressView().tint(colors.onPrimary)
                        } else if vm.cooldownSeconds > 0 {
                            LText("email_resend_in", vm.cooldownSeconds).mbFont(.labelLarge)
                        } else {
                            LText("email_send_code").mbFont(.labelLarge)
                        }
                    }
                    .frame(maxWidth: .infinity, minHeight: 52)
                    .foregroundColor(colors.onPrimary)
                    .background(colors.primary)
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                }
                .disabled(vm.sending || vm.cooldownSeconds > 0)

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
                Spacer()
            }
            .padding(.horizontal, 24)
            .frame(maxWidth: contentMaxWidth)
            .frame(maxWidth: .infinity)
        }
        .toolbar { ToolbarItem(placement: .navigationBarTrailing) { LoginSettingsMenu() } }
        .navigationBarTitleDisplayMode(.inline)
    }

    /// A neutral outlined federated-sign-in button (matches the Google one), used
    /// for Google / Apple / WeChat. Disabled while any sign-in or send is busy.
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
