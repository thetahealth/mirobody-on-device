import Foundation

/// Mirrors backend `sending_interval` in user/email.py (default 60s).
let sendCodeCooldownSeconds = 60

/// Pulls a user-facing message out of an error, preferring the backend's message
/// (matches Android's `t.message ?: fallback`; these fallbacks are intentionally
/// English-only, as in the Kotlin view models).
private func describe(_ error: Error, fallback: String) -> String {
    if case .api(_, let serverMessage?, _) = error.toAppError(), !serverMessage.isEmpty {
        return serverMessage
    }
    return fallback
}

@MainActor
final class EmailViewModel: ObservableObject {
    @Published var email: String
    @Published private(set) var sending = false
    @Published private(set) var sent = false
    @Published var error: String?
    @Published private(set) var cooldownSeconds = 0
    @Published private(set) var googleSigningIn = false
    @Published private(set) var appleSigningIn = false
    @Published private(set) var wechatSigningIn = false
    @Published private(set) var githubSigningIn = false
    @Published private(set) var xSigningIn = false

    private let repo: AuthRepository
    private let googleRepo: GoogleAuthRepository
    private let appleRepo: AppleAuthRepository
    private let wechatRepo: WeChatAuthRepository
    private let githubRepo: GitHubAuthRepository
    private let xRepo: XAuthRepository
    private var cooldownTask: Task<Void, Never>?

    init(repo: AuthRepository,
         googleRepo: GoogleAuthRepository,
         appleRepo: AppleAuthRepository,
         wechatRepo: WeChatAuthRepository,
         githubRepo: GitHubAuthRepository,
         xRepo: XAuthRepository,
         initialEmail: String?) {
        self.repo = repo
        self.googleRepo = googleRepo
        self.appleRepo = appleRepo
        self.wechatRepo = wechatRepo
        self.githubRepo = githubRepo
        self.xRepo = xRepo
        self.email = initialEmail ?? ""
    }

    var googleAvailable: Bool { googleRepo.isAvailable }
    var appleAvailable: Bool { appleRepo.isAvailable }
    var wechatAvailable: Bool { wechatRepo.isAvailable }
    var githubAvailable: Bool { githubRepo.isAvailable }
    var xAvailable: Bool { xRepo.isAvailable }
    var anyBusy: Bool { sending || googleSigningIn || appleSigningIn || wechatSigningIn || githubSigningIn || xSigningIn }

    func onEmailChange(_ value: String) {
        email = value
        error = nil
        sent = false
    }

    func sendCode(onSent: @escaping (String) -> Void) {
        let trimmed = email.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty { error = "Email is required"; return }
        if cooldownSeconds > 0 { return }
        sending = true
        error = nil
        Task {
            do {
                try await repo.sendCode(email: trimmed)
                sending = false
                sent = true
                startCooldown()
                onSent(trimmed)
            } catch {
                sending = false
                self.error = describe(error, fallback: "Failed to send code")
            }
        }
    }

    func signInWithGoogle(onSignedIn: @escaping () -> Void) {
        if googleSigningIn { return }
        googleSigningIn = true
        error = nil
        Task {
            do {
                try await googleRepo.signInWithGoogle()
                googleSigningIn = false
                onSignedIn()
            } catch {
                googleSigningIn = false
                self.error = describe(error, fallback: "Google sign-in failed")
            }
        }
    }

    func signInWithApple(onSignedIn: @escaping () -> Void) {
        if appleSigningIn { return }
        appleSigningIn = true
        error = nil
        Task {
            do {
                try await appleRepo.signInWithApple()
                appleSigningIn = false
                onSignedIn()
            } catch {
                appleSigningIn = false
                self.error = describe(error, fallback: "Apple sign-in failed")
            }
        }
    }

    func signInWithWeChat(onSignedIn: @escaping () -> Void) {
        if wechatSigningIn { return }
        wechatSigningIn = true
        error = nil
        Task {
            do {
                try await wechatRepo.signInWithWeChat()
                wechatSigningIn = false
                onSignedIn()
            } catch {
                wechatSigningIn = false
                self.error = describe(error, fallback: "WeChat sign-in failed")
            }
        }
    }

    func signInWithGithub(onSignedIn: @escaping () -> Void) {
        if githubSigningIn { return }
        githubSigningIn = true
        error = nil
        Task {
            do {
                try await githubRepo.signInWithGithub()
                githubSigningIn = false
                onSignedIn()
            } catch {
                githubSigningIn = false
                self.error = describe(error, fallback: "GitHub sign-in failed")
            }
        }
    }

    func signInWithX(onSignedIn: @escaping () -> Void) {
        if xSigningIn { return }
        xSigningIn = true
        error = nil
        Task {
            do {
                try await xRepo.signInWithX()
                xSigningIn = false
                onSignedIn()
            } catch {
                xSigningIn = false
                self.error = describe(error, fallback: "X sign-in failed")
            }
        }
    }

    private func startCooldown() {
        cooldownTask?.cancel()
        cooldownTask = Task {
            cooldownSeconds = sendCodeCooldownSeconds
            while cooldownSeconds > 0 {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                if Task.isCancelled { return }
                cooldownSeconds -= 1
            }
        }
    }
}

@MainActor
final class VerifyViewModel: ObservableObject {
    static let codeLength = 6

    @Published private(set) var email: String
    @Published private(set) var code = ""
    @Published private(set) var verifying = false
    @Published private(set) var resending = false
    @Published var error: String?
    @Published private(set) var resendCooldownSeconds = 0

    private let repo: AuthRepository
    private var cooldownTask: Task<Void, Never>?

    init(repo: AuthRepository, email: String) {
        self.repo = repo
        self.email = email
        startCooldown()   // a code was just sent from EmailView
    }

    func onCodeChange(_ value: String) {
        code = String(value.filter(\.isNumber).prefix(Self.codeLength))
        error = nil
    }

    func verify(onSuccess: @escaping () -> Void) {
        if verifying { return }
        if code.count < 4 { error = "Enter the code from your email"; return }
        verifying = true
        error = nil
        Task {
            do {
                try await repo.verifyCode(email: email, code: code)
                verifying = false
                onSuccess()
            } catch {
                // Clear the code so the user can retype without backspacing 6 digits.
                verifying = false
                code = ""
                self.error = describe(error, fallback: "Verification failed")
            }
        }
    }

    func resend() {
        if resendCooldownSeconds > 0 { return }
        resending = true
        error = nil
        Task {
            do {
                try await repo.sendCode(email: email)
                resending = false
                startCooldown()
            } catch {
                resending = false
                self.error = describe(error, fallback: "Failed to resend")
            }
        }
    }

    private func startCooldown() {
        cooldownTask?.cancel()
        cooldownTask = Task {
            resendCooldownSeconds = sendCodeCooldownSeconds
            while resendCooldownSeconds > 0 {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                if Task.isCancelled { return }
                resendCooldownSeconds -= 1
            }
        }
    }
}
