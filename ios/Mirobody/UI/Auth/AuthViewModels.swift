import Foundation

/// Mirrors backend `sending_interval` in user/email.py (default 60s).
let sendCodeCooldownSeconds = 60

/// Length of the emailed one-time code, as the backend issues it.
let oneTimeCodeLength = 6

/// The backend's own message when it sent one (Android reads `t.message` the same
/// way), else nil so the caller's localized fallback shows instead.
private func serverMessage(_ error: Error) -> String? {
    if case .api(_, let message?, _) = error.toAppError(), !message.isEmpty {
        return message
    }
    return nil
}

/// The one status line under the sign-in form (login.js's `status` paragraph), written
/// by every flow — email and federated alike. Progress and confirmations are `.info`;
/// failures are `.error`, which show the server's own message when it sent one and fall
/// back to `key` otherwise.
///
/// The cases carry a string key rather than resolved text so the line is localized by
/// whoever renders it — and re-localizes itself when the user switches language
/// mid-screen, which a String snapshotted in the view model would not.
enum EmailStatus {
    case idle
    case info(key: String, arg: String? = nil)
    case error(key: String, message: String? = nil)
}

/// The whole email sign-in screen's state: `htdoc/src/login.js`'s staircase, each step
/// unlocking the next. There is no separate verify screen (nor a VerifyViewModel) — the
/// code and Sign in live on the same card as the address, as they do on the web and on
/// Android.
@MainActor
final class EmailViewModel: ObservableObject {
    @Published var email: String
    @Published private(set) var code = ""
    @Published private(set) var sending = false
    @Published private(set) var verifying = false
    /// Lowercased address the last code was sent to, "" until one is. Comparing it
    /// against the field (rather than a plain `sent` flag) is what re-locks the code
    /// field when the address is edited, and unlocks it again on a change back.
    @Published private(set) var codeSentTo = ""
    /// The code came back rejected: tint the field until the next edit.
    @Published private(set) var codeRejected = false
    @Published private(set) var status: EmailStatus = .idle
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
    var anyBusy: Bool {
        sending || verifying || googleSigningIn || appleSigningIn
            || wechatSigningIn || githubSigningIn || xSigningIn
    }

    /// Step 1 of login.js's staircase: a valid address unlocks "Send code". At least
    /// `*@*.*` — stricter than the server's lenient `normalize_email` (which would
    /// accept a single-label domain like "user@demo"), so a typo dies here instead of
    /// costing a sent code.
    var emailValid: Bool {
        trimmedEmail.range(of: "^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$", options: .regularExpression) != nil
    }

    /// Step 2: a code went to the address that is in the field right now.
    var codeSent: Bool { !codeSentTo.isEmpty && codeSentTo == trimmedEmail.lowercased() }

    /// "Send code" is free: a valid address, no request in flight, no cooldown left.
    var canSendCode: Bool { emailValid && !sending && cooldownSeconds == 0 }

    /// Step 3: the whole staircase — valid address, code sent to it, all six digits.
    var canVerify: Bool { emailValid && codeSent && code.count == oneTimeCodeLength && !verifying }

    private var trimmedEmail: String { email.trimmingCharacters(in: .whitespaces) }

    func onEmailChange(_ value: String) {
        // Nothing else to reset: editing the address re-locks the code field on its own
        // (see `codeSent`), and login.js leaves the status line alone until the next
        // action writes it.
        email = value
    }

    func onCodeChange(_ value: String) {
        // Digits only, six at most — and any edit drops the rejection tint.
        code = String(value.filter(\.isNumber).prefix(oneTimeCodeLength))
        codeRejected = false
    }

    func sendCode() {
        let address = trimmedEmail
        // Both guards are also enforced by the button's disabled state; they stand here
        // so a stray call can't spend a code on a malformed address or beat the cooldown.
        if !emailValid {
            status = .error(key: "email_required")
            return
        }
        if sending || cooldownSeconds > 0 { return }
        sending = true
        status = .info(key: "email_sending_code")
        Task {
            do {
                try await repo.sendCode(email: address)
                // A new code means a clean field: drop whatever was typed for the
                // previous one, then unlock it (codeSentTo) and confirm on the status
                // line. The cooldown owns the button from here.
                sending = false
                codeSentTo = address.lowercased()
                code = ""
                codeRejected = false
                status = .info(key: "verify_sent_to", arg: address)
                startCooldown()
            } catch {
                sending = false
                status = .error(key: "email_send_failed", message: serverMessage(error))
            }
        }
    }

    func verify(onSignedIn: @escaping () -> Void) {
        if verifying { return }
        if code.isEmpty {
            status = .error(key: "email_enter_code")
            return
        }
        verifying = true
        codeRejected = false
        status = .info(key: "email_verifying")
        let address = trimmedEmail
        Task {
            do {
                try await repo.verifyCode(email: address, code: code)
                onSignedIn()
            } catch {
                // Keep the digits and tint the field instead of clearing it, as login.js
                // does: one mistyped digit is then a backspace away rather than a full
                // retype.
                verifying = false
                codeRejected = true
                status = .error(key: "email_verify_failed", message: serverMessage(error))
            }
        }
    }

    func signInWithGoogle(onSignedIn: @escaping () -> Void) {
        if googleSigningIn { return }
        googleSigningIn = true
        status = .idle
        Task {
            do {
                try await googleRepo.signInWithGoogle()
                googleSigningIn = false
                onSignedIn()
            } catch {
                googleSigningIn = false
                status = .error(key: "auth_google_failed", message: serverMessage(error))
            }
        }
    }

    func signInWithApple(onSignedIn: @escaping () -> Void) {
        if appleSigningIn { return }
        appleSigningIn = true
        status = .idle
        Task {
            do {
                try await appleRepo.signInWithApple()
                appleSigningIn = false
                onSignedIn()
            } catch {
                appleSigningIn = false
                status = .error(key: "auth_apple_failed", message: serverMessage(error))
            }
        }
    }

    func signInWithWeChat(onSignedIn: @escaping () -> Void) {
        if wechatSigningIn { return }
        wechatSigningIn = true
        status = .idle
        Task {
            do {
                try await wechatRepo.signInWithWeChat()
                wechatSigningIn = false
                onSignedIn()
            } catch {
                wechatSigningIn = false
                status = .error(key: "auth_wechat_failed", message: serverMessage(error))
            }
        }
    }

    func signInWithGithub(onSignedIn: @escaping () -> Void) {
        if githubSigningIn { return }
        githubSigningIn = true
        status = .idle
        Task {
            do {
                try await githubRepo.signInWithGithub()
                githubSigningIn = false
                onSignedIn()
            } catch {
                githubSigningIn = false
                status = .error(key: "auth_github_failed", message: serverMessage(error))
            }
        }
    }

    func signInWithX(onSignedIn: @escaping () -> Void) {
        if xSigningIn { return }
        xSigningIn = true
        status = .idle
        Task {
            do {
                try await xRepo.signInWithX()
                xSigningIn = false
                onSignedIn()
            } catch {
                xSigningIn = false
                status = .error(key: "auth_x_failed", message: serverMessage(error))
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
