package ai.thetahealth.mirobody.ui.auth

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
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
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.ContentMaxWidth
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.LocalFontSizePreview
import ai.thetahealth.mirobody.ui.ProvideLocale
import ai.thetahealth.mirobody.ui.settings.BaseUrlDialog
import ai.thetahealth.mirobody.ui.settings.FontSizeDialog
import ai.thetahealth.mirobody.ui.settings.LanguageDialog
import kotlinx.coroutines.launch

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun EmailScreen(
    onCodeSent: (email: String) -> Unit,
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
    // ProvideLocale replaces LocalContext with a fresh ContextImpl from
    // createConfigurationContext, which has no Activity in its parent chain. The hosting
    // ComposeView (LocalView), however, was built with the Activity as its context.
    val activity = LocalView.current.context.findActivity()

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            CenterAlignedTopAppBar(
                colors = TopAppBarDefaults.centerAlignedTopAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background,
                ),
                title = {},
                actions = { LoginSettingsMenu(currentLanguage = language) },
            )
        },
    ) { padding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .imePadding(),
            contentAlignment = Alignment.TopCenter,
        ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .widthIn(max = ContentMaxWidth)
                .padding(horizontal = 24.dp),
        ) {
            Spacer(modifier = Modifier.height(32.dp))
            // Centered brand badge (the app icon) + title, matching the web sign-in.
            Box(
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .size(72.dp)
                    .clip(CircleShape)
                    .background(Color.White),
                contentAlignment = Alignment.Center,
            ) {
                Image(
                    painter = painterResource(R.drawable.ic_launcher_foreground),
                    contentDescription = null,
                    modifier = Modifier.size(72.dp),
                )
            }
            Spacer(modifier = Modifier.height(16.dp))
            Text(
                text = stringResource(R.string.email_title),
                style = MaterialTheme.typography.headlineMedium,
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.align(Alignment.CenterHorizontally),
            )
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = stringResource(R.string.email_subtitle),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .align(Alignment.CenterHorizontally)
                    .fillMaxWidth(),
            )
            Spacer(modifier = Modifier.height(28.dp))
            OutlinedTextField(
                value = state.email,
                onValueChange = vm::onEmailChange,
                label = { Text(stringResource(R.string.email_label)) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                isError = state.error != null,
                supportingText = state.error?.let { { Text(it) } },
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(10.dp),
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = MaterialTheme.colorScheme.primary,
                    unfocusedBorderColor = MaterialTheme.colorScheme.outlineVariant,
                ),
            )
            Spacer(modifier = Modifier.height(20.dp))
            Button(
                onClick = { vm.sendCode(onSent = onCodeSent) },
                enabled = !state.sending && state.cooldownSeconds == 0,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp),
                shape = RoundedCornerShape(10.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.primary,
                    contentColor = MaterialTheme.colorScheme.onPrimary,
                ),
            ) {
                when {
                    state.sending -> {
                        Box(modifier = Modifier.size(20.dp), contentAlignment = Alignment.Center) {
                            CircularProgressIndicator(
                                strokeWidth = 2.dp,
                                color = MaterialTheme.colorScheme.onPrimary,
                            )
                        }
                    }
                    state.cooldownSeconds > 0 -> {
                        Text(stringResource(R.string.email_resend_in, state.cooldownSeconds), style = MaterialTheme.typography.labelLarge)
                    }
                    else -> {
                        Text(stringResource(R.string.email_send_code), style = MaterialTheme.typography.labelLarge)
                    }
                }
            }
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
            // The Compose host is always a ComponentActivity in this app; guard
            // anyway so previews don't crash. `busy` disables all federated
            // buttons while any one is in flight.
            val busy = state.sending || state.googleSigningIn ||
                state.appleSigningIn || state.wechatSigningIn || state.githubSigningIn ||
                state.xSigningIn
            // Show a federated button only when the server advertises that provider.
            // google/apple/wechat have /mirobody.json flags; github/x have none yet, so
            // they stay visible (no signal to hide them).
            if (serverConfig?.isGoogleLoginOn == true) {
                SocialButton(
                    label = stringResource(R.string.auth_continue_with_google),
                    loading = state.googleSigningIn,
                    enabled = activity != null && !busy,
                    onClick = { activity?.let { vm.signInWithGoogle(it, onSignedIn) } },
                )
                Spacer(modifier = Modifier.height(12.dp))
            }
            if (serverConfig?.isAppleLoginOn == true) {
                SocialButton(
                    label = stringResource(R.string.auth_continue_with_apple),
                    loading = state.appleSigningIn,
                    enabled = activity != null && !busy,
                    onClick = { activity?.let { vm.signInWithApple(it, onSignedIn) } },
                )
                Spacer(modifier = Modifier.height(12.dp))
            }
            if (serverConfig?.isWechatLoginOn == true) {
                SocialButton(
                    label = stringResource(R.string.auth_continue_with_wechat),
                    loading = state.wechatSigningIn,
                    enabled = !busy,
                    onClick = { vm.signInWithWeChat(onSignedIn) },
                )
                Spacer(modifier = Modifier.height(12.dp))
            }
            SocialButton(
                label = stringResource(R.string.auth_continue_with_github),
                loading = state.githubSigningIn,
                enabled = activity != null && !busy,
                onClick = { activity?.let { vm.signInWithGithub(it, onSignedIn) } },
            )
            Spacer(modifier = Modifier.height(12.dp))
            SocialButton(
                label = stringResource(R.string.auth_continue_with_x),
                loading = state.xSigningIn,
                enabled = activity != null && !busy,
                onClick = { activity?.let { vm.signInWithX(it, onSignedIn) } },
            )
        }
        }
    }
}

@Composable
private fun SocialButton(
    label: String,
    loading: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    OutlinedButton(
        onClick = onClick,
        enabled = enabled,
        modifier = Modifier
            .fillMaxWidth()
            .height(52.dp),
        shape = RoundedCornerShape(10.dp),
    ) {
        if (loading) {
            Box(modifier = Modifier.size(20.dp), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(
                    strokeWidth = 2.dp,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
        } else {
            Text(text = label, style = MaterialTheme.typography.labelLarge)
        }
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}

/**
 * Pre-auth settings menu shared by the login + verify screens. Mirrors the web
 * client's menu before sign-in: Language, Font size, Backend (the care-circle /
 * health / sign-out items only appear once authenticated, in the chat menu).
 */
@Composable
internal fun LoginSettingsMenu(currentLanguage: String) {
    val container = LocalAppContainer.current
    val scope = rememberCoroutineScope()
    val fontOffset by container.settings.fontSizeOffset.collectAsState(initial = 0)
    val fontSizePreview = LocalFontSizePreview.current
    var expanded by remember { mutableStateOf(false) }
    var showLanguageDialog by remember { mutableStateOf(false) }
    var showFontSizeDialog by remember { mutableStateOf(false) }
    var showBackendDialog by remember { mutableStateOf(false) }
    IconButton(onClick = { expanded = true }) {
        Icon(
            Icons.Outlined.Settings,
            contentDescription = stringResource(R.string.common_settings),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
        ProvideLocale(currentLanguage) {
            DropdownMenuItem(
                text = { Text(stringResource(R.string.chat_language)) },
                onClick = {
                    expanded = false
                    showLanguageDialog = true
                },
            )
            DropdownMenuItem(
                text = { Text(stringResource(R.string.chat_font_size)) },
                onClick = {
                    expanded = false
                    showFontSizeDialog = true
                },
            )
            DropdownMenuItem(
                text = { Text(stringResource(R.string.chat_backend)) },
                onClick = {
                    expanded = false
                    showBackendDialog = true
                },
            )
        }
    }
    if (showLanguageDialog) {
        LanguageDialog(
            current = currentLanguage,
            onPick = { code ->
                scope.launch { container.settings.setLanguage(code) }
                showLanguageDialog = false
            },
            onDismiss = { showLanguageDialog = false },
        )
    }
    if (showFontSizeDialog) {
        FontSizeDialog(
            currentLanguage = currentLanguage,
            current = fontOffset,
            onPreview = { offset -> fontSizePreview.value = offset },
            onPick = { offset ->
                scope.launch { container.settings.setFontSizeOffset(offset) }
                showFontSizeDialog = false
            },
            onDismiss = {
                fontSizePreview.value = null
                showFontSizeDialog = false
            },
        )
    }
    if (showBackendDialog) {
        BaseUrlDialog(
            currentLanguage = currentLanguage,
            onDismiss = { showBackendDialog = false },
        )
    }
}
