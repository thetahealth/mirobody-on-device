import SwiftUI

/// One-time-code entry screen — mirrors `ui/auth/VerifyScreen.kt`. On success the
/// token lands in settings and `RootView` swaps to the chat screen, so there's no
/// explicit forward navigation here.
struct VerifyView: View {
    @Environment(\.mbColors) private var colors
    @Environment(\.dismiss) private var dismiss

    @StateObject private var vm: VerifyViewModel
    private let email: String

    @FocusState private var codeFocused: Bool

    init(container: AppContainer, email: String) {
        self.email = email
        _vm = StateObject(wrappedValue: VerifyViewModel(repo: container.authRepository, email: email))
    }

    private let codeLength = VerifyViewModel.codeLength

    var body: some View {
        ZStack(alignment: .top) {
            colors.background.ignoresSafeArea()
            VStack(alignment: .leading, spacing: 0) {
                Spacer().frame(height: 24)
                LText("verify_title").mbFont(.headlineMedium).foregroundColor(colors.onSurface)
                Spacer().frame(height: 8)
                LText("verify_sent_to", email).mbFont(.bodyMedium).foregroundColor(colors.onSurfaceVariant)
                Spacer().frame(height: 28)

                codeBoxes

                if let err = vm.error {
                    Spacer().frame(height: 8)
                    Text(err).mbFont(.bodySmall).foregroundColor(colors.error)
                }

                Spacer().frame(height: 20)
                Button(action: { vm.verify(onSuccess: {}) }) {
                    Group {
                        if vm.verifying { ProgressView().tint(colors.onPrimary) }
                        else { LText("verify_button").mbFont(.labelLarge) }
                    }
                    .frame(maxWidth: .infinity, minHeight: 52)
                    .foregroundColor(colors.onPrimary)
                    .background(colors.primary)
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                }
                .disabled(vm.verifying)

                Spacer().frame(height: 8)
                Button(action: { vm.resend() }) {
                    Group {
                        if vm.resending { LText("verify_resending") }
                        else if vm.resendCooldownSeconds > 0 { LText("email_resend_in", vm.resendCooldownSeconds) }
                        else { LText("verify_resend_code") }
                    }
                    .mbFont(.labelLarge).foregroundColor(colors.primary)
                }
                .frame(maxWidth: .infinity)
                .disabled(vm.resending || vm.resendCooldownSeconds > 0)

                Spacer()
            }
            .padding(.horizontal, 24)
            .frame(maxWidth: contentMaxWidth)
            .frame(maxWidth: .infinity)
        }
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarLeading) {
                Button { dismiss() } label: { Image(systemName: "chevron.backward") }
                    .tint(colors.onSurfaceVariant)
            }
            ToolbarItem(placement: .navigationBarTrailing) { LoginSettingsMenu() }
        }
        .onChange(of: vm.code) { newValue in
            if newValue.count == codeLength && !vm.verifying {
                vm.verify(onSuccess: {})
            }
        }
        .onAppear { codeFocused = true }
    }

    private var codeBoxes: some View {
        ZStack {
            HStack(spacing: 8) {
                ForEach(0..<codeLength, id: \.self) { i in
                    let chars = Array(vm.code)
                    let digit = i < chars.count ? String(chars[i]) : ""
                    let isFocusedSlot = vm.code.count == i
                    Text(digit)
                        .mbFont(.headlineSmall)
                        .foregroundColor(colors.onSurface)
                        .frame(maxWidth: .infinity)
                        .aspectRatio(0.8, contentMode: .fit)
                        .background(colors.surfaceContainerLow)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                        .overlay(
                            RoundedRectangle(cornerRadius: 8).stroke(
                                vm.error != nil ? colors.error
                                    : (isFocusedSlot ? colors.primary : colors.outlineVariant),
                                lineWidth: (isFocusedSlot || vm.error != nil) ? 2 : 1
                            )
                        )
                }
            }
            // Invisible field that owns the keyboard and feeds onCodeChange.
            TextField("", text: Binding(get: { vm.code }, set: vm.onCodeChange))
                .keyboardType(.numberPad)
                .focused($codeFocused)
                .opacity(0.001)
        }
        .contentShape(Rectangle())
        .onTapGesture { codeFocused = true }
    }
}
