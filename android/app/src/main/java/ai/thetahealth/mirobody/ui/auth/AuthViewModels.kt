package ai.thetahealth.mirobody.ui.auth

import android.app.Activity
import androidx.annotation.StringRes
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.data.auth.AppleAuthRepository
import ai.thetahealth.mirobody.data.auth.AuthRepository
import ai.thetahealth.mirobody.data.auth.GithubAuthRepository
import ai.thetahealth.mirobody.data.auth.GoogleAuthRepository
import ai.thetahealth.mirobody.data.auth.WechatAuthRepository
import ai.thetahealth.mirobody.data.auth.XAuthRepository
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

// Mirrors backend `sending_interval` in user/email.py (default 60s).
const val SEND_CODE_COOLDOWN_SECONDS = 60

/** Length of the emailed one-time code, as the backend issues it. */
const val CODE_LENGTH = 6

/**
 * A valid-looking address — the shape `htdoc/src/login.js` insists on before it will
 * spend a code: at least `*@*.*`, an '@' with non-empty, space-free parts on both
 * sides and a dot in the domain. Deliberately stricter than the server's lenient
 * `normalize_email` (which would accept a single-label domain like "user@demo"), so a
 * typo dies here instead of costing a sent code.
 */
internal val EMAIL_REGEX = Regex("^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$")

/**
 * The one status line under the sign-in form (login.js's `status` paragraph), written
 * by every flow — email and federated alike. Progress and confirmations are [Info];
 * failures are [Error], which show the server's own message when it sent one and fall
 * back to [res] otherwise.
 *
 * The cases carry a string id rather than a resolved string so the line is localized
 * by whoever renders it — and re-localizes itself when the user switches language
 * mid-screen, which a String snapshotted in the ViewModel would not.
 */
sealed interface EmailStatus {
    data object None : EmailStatus
    data class Info(@get:StringRes val res: Int, val arg: String? = null) : EmailStatus
    data class Error(@get:StringRes val res: Int, val message: String? = null) : EmailStatus
}

data class EmailUiState(
    val email: String = "",
    val code: String = "",
    val sending: Boolean = false,
    val verifying: Boolean = false,
    /**
     * Lowercased address the last code was sent to, "" until one is. Comparing it
     * against the field (rather than a plain `sent` flag) is what re-locks the code
     * field when the address is edited, and unlocks it again on a change back.
     */
    val codeSentTo: String = "",
    /** The code came back rejected: tint the field until the next edit. */
    val codeRejected: Boolean = false,
    val status: EmailStatus = EmailStatus.None,
    val cooldownSeconds: Int = 0,
    val googleSigningIn: Boolean = false,
    val appleSigningIn: Boolean = false,
    val wechatSigningIn: Boolean = false,
    val githubSigningIn: Boolean = false,
    val xSigningIn: Boolean = false,
) {
    /** Step 1 of login.js's staircase: a valid address unlocks "Send code". */
    val emailValid: Boolean get() = EMAIL_REGEX.matches(email.trim())

    /** Step 2: a code went to the address that is in the field right now. */
    val codeSent: Boolean get() = codeSentTo.isNotEmpty() && codeSentTo == email.trim().lowercase()

    /** "Send code" is free: a valid address, no request in flight, no cooldown left. */
    val canSendCode: Boolean get() = emailValid && !sending && cooldownSeconds == 0

    /** Step 3: the whole staircase — valid address, code sent to it, all six digits. */
    val canVerify: Boolean get() = emailValid && codeSent && code.length == CODE_LENGTH && !verifying
}

class EmailLoginViewModel(
    private val repo: AuthRepository,
    private val googleRepo: GoogleAuthRepository,
    private val appleRepo: AppleAuthRepository,
    private val wechatRepo: WechatAuthRepository,
    private val githubRepo: GithubAuthRepository,
    private val xRepo: XAuthRepository,
    initialEmail: String? = null,
) : ViewModel() {
    private val _state = MutableStateFlow(EmailUiState(email = initialEmail.orEmpty()))
    val state: StateFlow<EmailUiState> = _state.asStateFlow()
    private var cooldownJob: Job? = null

    fun onEmailChange(value: String) {
        // Nothing else to reset: editing the address re-locks the code field on its
        // own (see EmailUiState.codeSent), and login.js leaves the status line alone
        // until the next action writes it.
        _state.update { it.copy(email = value) }
    }

    fun onCodeChange(value: String) {
        // Digits only, six at most -- and any edit drops the rejection tint.
        val sanitized = value.filter { it.isDigit() }.take(CODE_LENGTH)
        _state.update { it.copy(code = sanitized, codeRejected = false) }
    }

    fun sendCode() {
        val email = _state.value.email.trim()
        // Both guards are also enforced by the button's enabled state; they stand here
        // so a stray call can't spend a code on a malformed address or beat the cooldown.
        if (!EMAIL_REGEX.matches(email)) {
            _state.update { it.copy(status = EmailStatus.Error(R.string.email_required)) }
            return
        }
        if (_state.value.sending || _state.value.cooldownSeconds > 0) return
        _state.update { it.copy(sending = true, status = EmailStatus.Info(R.string.email_sending_code)) }
        viewModelScope.launch {
            runCatching { repo.sendCode(email) }
                .onSuccess {
                    // A new code means a clean field: drop whatever was typed for the
                    // previous one, then unlock it (codeSentTo) and confirm on the
                    // status line. The cooldown owns the button from here.
                    _state.update {
                        it.copy(
                            sending = false,
                            codeSentTo = email.lowercase(),
                            code = "",
                            codeRejected = false,
                            status = EmailStatus.Info(R.string.verify_sent_to, email),
                        )
                    }
                    startCooldown()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(
                            sending = false,
                            status = EmailStatus.Error(R.string.email_send_failed, t.message),
                        )
                    }
                }
        }
    }

    fun verify(onSignedIn: () -> Unit) {
        val s = _state.value
        if (s.verifying) return
        if (s.code.isBlank()) {
            _state.update { it.copy(status = EmailStatus.Error(R.string.email_enter_code)) }
            return
        }
        _state.update {
            it.copy(verifying = true, codeRejected = false, status = EmailStatus.Info(R.string.email_verifying))
        }
        viewModelScope.launch {
            runCatching { repo.verifyCode(s.email.trim(), s.code) }
                .onSuccess { onSignedIn() }
                .onFailure { t ->
                    // Keep the digits and tint the field instead of clearing it, as
                    // login.js does: one mistyped digit is then a backspace away
                    // rather than a full retype.
                    _state.update {
                        it.copy(
                            verifying = false,
                            codeRejected = true,
                            status = EmailStatus.Error(R.string.email_verify_failed, t.message),
                        )
                    }
                }
        }
    }

    fun signInWithGoogle(activity: Activity, onSignedIn: () -> Unit) {
        if (_state.value.googleSigningIn) return
        _state.update { it.copy(googleSigningIn = true, status = EmailStatus.None) }
        viewModelScope.launch {
            runCatching { googleRepo.signInWithGoogle(activity) }
                .onSuccess {
                    _state.update { it.copy(googleSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(
                            googleSigningIn = false,
                            status = EmailStatus.Error(R.string.auth_google_failed, t.message),
                        )
                    }
                }
        }
    }

    fun signInWithApple(activity: Activity, onSignedIn: () -> Unit) {
        if (_state.value.appleSigningIn) return
        _state.update { it.copy(appleSigningIn = true, status = EmailStatus.None) }
        viewModelScope.launch {
            runCatching { appleRepo.signInWithApple(activity) }
                .onSuccess {
                    _state.update { it.copy(appleSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(
                            appleSigningIn = false,
                            status = EmailStatus.Error(R.string.auth_apple_failed, t.message),
                        )
                    }
                }
        }
    }

    fun signInWithWeChat(onSignedIn: () -> Unit) {
        if (_state.value.wechatSigningIn) return
        _state.update { it.copy(wechatSigningIn = true, status = EmailStatus.None) }
        viewModelScope.launch {
            runCatching { wechatRepo.signInWithWeChat() }
                .onSuccess {
                    _state.update { it.copy(wechatSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(
                            wechatSigningIn = false,
                            status = EmailStatus.Error(R.string.auth_wechat_failed, t.message),
                        )
                    }
                }
        }
    }

    fun signInWithGithub(activity: Activity, onSignedIn: () -> Unit) {
        if (_state.value.githubSigningIn) return
        _state.update { it.copy(githubSigningIn = true, status = EmailStatus.None) }
        viewModelScope.launch {
            runCatching { githubRepo.signInWithGithub(activity) }
                .onSuccess {
                    _state.update { it.copy(githubSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(
                            githubSigningIn = false,
                            status = EmailStatus.Error(R.string.auth_github_failed, t.message),
                        )
                    }
                }
        }
    }

    fun signInWithX(activity: Activity, onSignedIn: () -> Unit) {
        if (_state.value.xSigningIn) return
        _state.update { it.copy(xSigningIn = true, status = EmailStatus.None) }
        viewModelScope.launch {
            runCatching { xRepo.signInWithX(activity) }
                .onSuccess {
                    _state.update { it.copy(xSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(
                            xSigningIn = false,
                            status = EmailStatus.Error(R.string.auth_x_failed, t.message),
                        )
                    }
                }
        }
    }

    private fun startCooldown() {
        cooldownJob?.cancel()
        cooldownJob = viewModelScope.launch {
            _state.update { it.copy(cooldownSeconds = SEND_CODE_COOLDOWN_SECONDS) }
            while (_state.value.cooldownSeconds > 0) {
                delay(1000)
                _state.update { it.copy(cooldownSeconds = it.cooldownSeconds - 1) }
            }
        }
    }
}