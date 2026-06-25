package ai.thetahealth.mirobody.ui.auth

import android.app.Activity
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
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

data class EmailUiState(
    val email: String = "",
    val sending: Boolean = false,
    val sent: Boolean = false,
    val error: String? = null,
    val cooldownSeconds: Int = 0,
    val googleSigningIn: Boolean = false,
    val appleSigningIn: Boolean = false,
    val wechatSigningIn: Boolean = false,
    val githubSigningIn: Boolean = false,
    val xSigningIn: Boolean = false,
)

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
        _state.update { it.copy(email = value, error = null, sent = false) }
    }

    fun sendCode(onSent: (String) -> Unit) {
        val email = _state.value.email.trim()
        if (email.isBlank()) {
            _state.update { it.copy(error = "Email is required") }
            return
        }
        if (_state.value.cooldownSeconds > 0) return
        _state.update { it.copy(sending = true, error = null) }
        viewModelScope.launch {
            runCatching { repo.sendCode(email) }
                .onSuccess {
                    _state.update { it.copy(sending = false, sent = true) }
                    startCooldown()
                    onSent(email)
                }
                .onFailure { t ->
                    _state.update { it.copy(sending = false, error = t.message ?: "Failed to send code") }
                }
        }
    }

    fun signInWithGoogle(activity: Activity, onSignedIn: () -> Unit) {
        if (_state.value.googleSigningIn) return
        _state.update { it.copy(googleSigningIn = true, error = null) }
        viewModelScope.launch {
            runCatching { googleRepo.signInWithGoogle(activity) }
                .onSuccess {
                    _state.update { it.copy(googleSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(googleSigningIn = false, error = t.message ?: "Google sign-in failed")
                    }
                }
        }
    }

    fun signInWithApple(activity: Activity, onSignedIn: () -> Unit) {
        if (_state.value.appleSigningIn) return
        _state.update { it.copy(appleSigningIn = true, error = null) }
        viewModelScope.launch {
            runCatching { appleRepo.signInWithApple(activity) }
                .onSuccess {
                    _state.update { it.copy(appleSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(appleSigningIn = false, error = t.message ?: "Apple sign-in failed")
                    }
                }
        }
    }

    fun signInWithWeChat(onSignedIn: () -> Unit) {
        if (_state.value.wechatSigningIn) return
        _state.update { it.copy(wechatSigningIn = true, error = null) }
        viewModelScope.launch {
            runCatching { wechatRepo.signInWithWeChat() }
                .onSuccess {
                    _state.update { it.copy(wechatSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(wechatSigningIn = false, error = t.message ?: "WeChat sign-in failed")
                    }
                }
        }
    }

    fun signInWithGithub(activity: Activity, onSignedIn: () -> Unit) {
        if (_state.value.githubSigningIn) return
        _state.update { it.copy(githubSigningIn = true, error = null) }
        viewModelScope.launch {
            runCatching { githubRepo.signInWithGithub(activity) }
                .onSuccess {
                    _state.update { it.copy(githubSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(githubSigningIn = false, error = t.message ?: "GitHub sign-in failed")
                    }
                }
        }
    }

    fun signInWithX(activity: Activity, onSignedIn: () -> Unit) {
        if (_state.value.xSigningIn) return
        _state.update { it.copy(xSigningIn = true, error = null) }
        viewModelScope.launch {
            runCatching { xRepo.signInWithX(activity) }
                .onSuccess {
                    _state.update { it.copy(xSigningIn = false) }
                    onSignedIn()
                }
                .onFailure { t ->
                    _state.update {
                        it.copy(xSigningIn = false, error = t.message ?: "X sign-in failed")
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

data class VerifyUiState(
    val email: String,
    val code: String = "",
    val verifying: Boolean = false,
    val resending: Boolean = false,
    val error: String? = null,
    val resendCooldownSeconds: Int = 0,
)

class VerifyCodeViewModel(
    private val repo: AuthRepository,
    email: String,
) : ViewModel() {
    private val _state = MutableStateFlow(VerifyUiState(email = email))
    val state: StateFlow<VerifyUiState> = _state.asStateFlow()
    private var cooldownJob: Job? = null

    init {
        // User just had a code sent (from EmailScreen) right before landing here.
        startCooldown()
    }

    fun onCodeChange(value: String) {
        val sanitized = value.filter { it.isDigit() }.take(6)
        _state.update { it.copy(code = sanitized, error = null) }
    }

    fun verify(onSuccess: () -> Unit) {
        val s = _state.value
        if (s.verifying) return
        if (s.code.length < 4) {
            _state.update { it.copy(error = "Enter the code from your email") }
            return
        }
        _state.update { it.copy(verifying = true, error = null) }
        viewModelScope.launch {
            runCatching { repo.verifyCode(s.email, s.code) }
                .onSuccess {
                    _state.update { it.copy(verifying = false) }
                    onSuccess()
                }
                .onFailure { t ->
                    // Clear the code so user can retype without manually backspacing 6 digits.
                    _state.update { it.copy(verifying = false, code = "", error = t.message ?: "Verification failed") }
                }
        }
    }

    fun resend() {
        if (_state.value.resendCooldownSeconds > 0) return
        _state.update { it.copy(resending = true, error = null) }
        viewModelScope.launch {
            runCatching { repo.sendCode(_state.value.email) }
                .onSuccess {
                    _state.update { it.copy(resending = false) }
                    startCooldown()
                }
                .onFailure { t -> _state.update { it.copy(resending = false, error = t.message ?: "Failed to resend") } }
        }
    }

    private fun startCooldown() {
        cooldownJob?.cancel()
        cooldownJob = viewModelScope.launch {
            _state.update { it.copy(resendCooldownSeconds = SEND_CODE_COOLDOWN_SECONDS) }
            while (_state.value.resendCooldownSeconds > 0) {
                delay(1000)
                _state.update { it.copy(resendCooldownSeconds = it.resendCooldownSeconds - 1) }
            }
        }
    }
}