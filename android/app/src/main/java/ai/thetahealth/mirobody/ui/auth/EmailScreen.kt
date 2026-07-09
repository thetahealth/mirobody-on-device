package ai.thetahealth.mirobody.ui.auth

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.ContentMaxWidth
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.settings.AppSettingsMenu

// Match the server's lenient rule (normalize_email): an '@' with non-empty,
// space-free parts on both sides. No dot required, mirroring login.js.
private val EMAIL_REGEX = Regex("^[^\\s@]+@[^\\s@]+$")

/**
 * Single-screen sign-in, mirroring the web client (`htdoc/src/login.js`): a
 * centered brand block, the configured social providers, an OR divider, then
 * email + one-time code (a code field with an inline Send code and a primary
 * Sign in) — no separate verify screen.
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun EmailScreen(
    onSignedIn: () -> Unit,
) {
    val container = LocalAppContainer.current
    val lastEmail by container.settings.lastEmail.collectAsState(initial = null)
    val language by container.settings.language.collectAsState(initial = "en")
    val vm: EmailLoginViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                EmailLoginViewModel(
                    repo = container.authRepository,
                    googleRepo = container.googleAuthRepository,
                    appleRepo = container.appleAuthRepository,
                    wechatRepo = container.wechatAuthRepository,
                    githubRepo = container.githubAuthRepository,
                    xRepo = container.xAuthRepository,
                    initialEmail = lastEmail,
                )
            }
        },
    )
    val state by vm.state.collectAsState()
    // Server-advertised provider availability (/mirobody.json). Adaptive sign-in:
    // only show a federated button the server actually configured — mirrors the web
    // client and avoids e.g. a WeChat button on a server without WeChat.
    val serverConfig by container.serverConfigStore.config.collectAsState()
    // Resolve the Activity from the hosting ComposeView's context (needed to launch
    // the federated sign-in flows).
    val activity = LocalView.current.context.findActivity()

    val codeFocus = remember { FocusRequester() }
    // Focus the code field once a code has been sent, and auto-submit on 6 digits.
    LaunchedEffect(state.sent) { if (state.sent) codeFocus.requestFocus() }
    LaunchedEffect(state.code) {
        if (state.code.length == 6 && !state.verifying) vm.verify(onSignedIn)
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            CenterAlignedTopAppBar(
                colors = TopAppBarDefaults.centerAlignedTopAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background,
                ),
                title = {},
                actions = { AppSettingsMenu(currentLanguage = language) },
            )
        },
    ) { padding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                // Only take the app-bar/status-bar inset from the Scaffold here; the
                // bottom navigation-bar inset is applied explicitly so the scrollable
                // content can bring the Sign in button fully above the nav bar
                // (edge-to-edge is on — see MainActivity.enableEdgeToEdge).
                .padding(top = padding.calculateTopPadding())
                .navigationBarsPadding()
                .imePadding(),
            contentAlignment = Alignment.TopCenter,
        ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .widthIn(max = ContentMaxWidth)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 24.dp),
        ) {
            Spacer(modifier = Modifier.height(24.dp))
            // -- centered brand block: logo + serif wordmark + subtitle ----------
            Image(
                painter = painterResource(R.drawable.ic_mirobody_logo),
                contentDescription = null,
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .size(width = 58.dp, height = 60.dp),
            )
            Spacer(modifier = Modifier.height(16.dp))
            Text(
                text = "Mirobody",
                style = MaterialTheme.typography.displaySmall.copy(
                    fontFamily = FontFamily.Serif,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 44.sp,
                    letterSpacing = (-0.5).sp,
                ),
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.align(Alignment.CenterHorizontally),
            )
            Spacer(modifier = Modifier.height(12.dp))
            Text(
                text = stringResource(R.string.login_continue),
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .fillMaxWidth(),
            )
            Spacer(modifier = Modifier.height(28.dp))

            // -- social providers: a compact icon row (matches the web client on
            // mobile — a stack of labeled buttons is too tall for a phone). The
            // Compose host is always a ComponentActivity in this app; guard anyway
            // so previews don't crash. `busy` disables every provider while any one
            // is in flight.
            val busy = state.sending || state.verifying || state.googleSigningIn ||
                state.appleSigningIn || state.wechatSigningIn || state.githubSigningIn ||
                state.xSigningIn
            val markTint = MaterialTheme.colorScheme.onSurface   // adaptive black/white marks
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterHorizontally),
                verticalArrangement = Arrangement.spacedBy(12.dp),
                modifier = Modifier.fillMaxWidth(),
            ) {
                // Show a federated icon only when the server advertises that provider.
                // google/apple/wechat have /mirobody.json flags; github/x have none
                // yet, so they stay visible (no signal to hide them).
                if (serverConfig?.isGoogleLoginOn == true) {
                    SocialIconButton(
                        iconRes = R.drawable.ic_provider_google,
                        contentDescription = stringResource(R.string.auth_continue_with_google),
                        tint = null,   // official four-color "G" — keep its own fills
                        loading = state.googleSigningIn,
                        enabled = activity != null && !busy,
                        onClick = { activity?.let { vm.signInWithGoogle(it, onSignedIn) } },
                    )
                }
                if (serverConfig?.isAppleLoginOn == true) {
                    SocialIconButton(
                        iconRes = R.drawable.ic_provider_apple,
                        contentDescription = stringResource(R.string.auth_continue_with_apple),
                        tint = markTint,
                        loading = state.appleSigningIn,
                        enabled = activity != null && !busy,
                        onClick = { activity?.let { vm.signInWithApple(it, onSignedIn) } },
                    )
                }
                if (serverConfig?.isWechatLoginOn == true) {
                    SocialIconButton(
                        iconRes = R.drawable.ic_provider_wechat,
                        contentDescription = stringResource(R.string.auth_continue_with_wechat),
                        tint = Color(0xFF07C160),   // WeChat green, fixed on both themes
                        loading = state.wechatSigningIn,
                        enabled = !busy,
                        onClick = { vm.signInWithWeChat(onSignedIn) },
                    )
                }
                SocialIconButton(
                    iconRes = R.drawable.ic_provider_github,
                    contentDescription = stringResource(R.string.auth_continue_with_github),
                    tint = markTint,
                    loading = state.githubSigningIn,
                    enabled = activity != null && !busy,
                    onClick = { activity?.let { vm.signInWithGithub(it, onSignedIn) } },
                )
                SocialIconButton(
                    iconRes = R.drawable.ic_provider_x,
                    contentDescription = stringResource(R.string.auth_continue_with_x),
                    tint = markTint,
                    loading = state.xSigningIn,
                    enabled = activity != null && !busy,
                    onClick = { activity?.let { vm.signInWithX(it, onSignedIn) } },
                )
            }

            // -- OR divider (always present: github/x are unconditional) --------
            Spacer(modifier = Modifier.height(24.dp))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                HorizontalDivider(
                    modifier = Modifier.weight(1f),
                    color = MaterialTheme.colorScheme.outlineVariant,
                )
                Text(
                    text = stringResource(R.string.auth_or),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 12.dp),
                )
                HorizontalDivider(
                    modifier = Modifier.weight(1f),
                    color = MaterialTheme.colorScheme.outlineVariant,
                )
            }
            Spacer(modifier = Modifier.height(20.dp))

            // -- email + one-time code ------------------------------------------
            val emailValid = EMAIL_REGEX.matches(state.email.trim())
            OutlinedTextField(
                value = state.email,
                onValueChange = vm::onEmailChange,
                label = { Text(stringResource(R.string.email_label)) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                isError = state.error != null,
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(10.dp),
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = MaterialTheme.colorScheme.primary,
                    unfocusedBorderColor = MaterialTheme.colorScheme.outlineVariant,
                ),
            )
            Spacer(modifier = Modifier.height(12.dp))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                modifier = Modifier.fillMaxWidth(),
            ) {
                OutlinedTextField(
                    value = state.code,
                    onValueChange = vm::onCodeChange,
                    label = { Text(stringResource(R.string.verification_code)) },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
                    modifier = Modifier
                        .weight(1f)
                        .focusRequester(codeFocus),
                    shape = RoundedCornerShape(10.dp),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = MaterialTheme.colorScheme.primary,
                        unfocusedBorderColor = MaterialTheme.colorScheme.outlineVariant,
                    ),
                )
                OutlinedButton(
                    onClick = { vm.sendCode() },
                    enabled = emailValid && !state.sending && state.cooldownSeconds == 0,
                    modifier = Modifier.height(56.dp),
                    shape = RoundedCornerShape(10.dp),
                ) {
                    when {
                        state.sending -> {
                            Box(modifier = Modifier.size(20.dp), contentAlignment = Alignment.Center) {
                                CircularProgressIndicator(
                                    strokeWidth = 2.dp,
                                    color = MaterialTheme.colorScheme.primary,
                                )
                            }
                        }
                        state.cooldownSeconds > 0 -> {
                            Text(stringResource(R.string.email_resend_in, state.cooldownSeconds))
                        }
                        else -> {
                            Text(stringResource(R.string.email_send_code))
                        }
                    }
                }
            }
            Spacer(modifier = Modifier.height(16.dp))
            Button(
                onClick = { vm.verify(onSignedIn) },
                enabled = !state.verifying,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp),
                shape = RoundedCornerShape(10.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.primary,
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                ),
            ) {
                if (state.verifying) {
                    Box(modifier = Modifier.size(20.dp), contentAlignment = Alignment.Center) {
                        CircularProgressIndicator(
                            strokeWidth = 2.dp,
                            color = MaterialTheme.colorScheme.onPrimary,
                        )
                    }
                } else {
                    Text(stringResource(R.string.email_title), style = MaterialTheme.typography.labelLarge)
                }
            }

            // Shared status line: the error takes precedence, else confirm the send.
            Spacer(modifier = Modifier.height(8.dp))
            val statusText = state.error
                ?: if (state.sent) stringResource(R.string.verify_sent_to, state.email) else null
            Text(
                text = statusText.orEmpty(),
                style = MaterialTheme.typography.bodySmall,
                color = if (state.error != null) MaterialTheme.colorScheme.error
                        else MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .fillMaxWidth(),
            )
            Spacer(modifier = Modifier.height(24.dp))
        }
        }
    }
}

/** A 52dp square icon-only federated-sign-in button (the web client's mobile
 *  provider row). `tint` recolors a single-color mark; pass null to keep a
 *  multi-color mark's own fills (Google's "G"). */
@Composable
private fun SocialIconButton(
    iconRes: Int,
    contentDescription: String,
    tint: Color?,
    loading: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .size(52.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(MaterialTheme.colorScheme.surfaceContainerLow)
            .border(
                width = 1.dp,
                color = MaterialTheme.colorScheme.outlineVariant,
                shape = RoundedCornerShape(12.dp),
            )
            .clickable(enabled = enabled, onClick = onClick)
            .alpha(if (enabled) 1f else 0.5f),
        contentAlignment = Alignment.Center,
    ) {
        if (loading) {
            CircularProgressIndicator(
                modifier = Modifier.size(20.dp),
                strokeWidth = 2.dp,
                color = MaterialTheme.colorScheme.primary,
            )
        } else {
            Image(
                painter = painterResource(iconRes),
                contentDescription = contentDescription,
                modifier = Modifier.size(22.dp),
                colorFilter = tint?.let { ColorFilter.tint(it) },
            )
        }
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

// The app settings menu (gear) now lives in ui/settings/AppSettingsMenu.kt so
// the login and chat screens share one identical menu.
