package ai.thetahealth.mirobody.ui.auth

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
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
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.ContentMaxWidth
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.ProvideLocale
import ai.thetahealth.mirobody.ui.settings.BaseUrlDialog

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
                actions = { BackendOnlyMenu(currentLanguage = language) },
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
            Spacer(modifier = Modifier.height(24.dp))
            Text(
                text = stringResource(R.string.email_title),
                style = MaterialTheme.typography.headlineMedium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                text = stringResource(R.string.email_subtitle),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
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
            SocialButton(
                label = stringResource(R.string.auth_continue_with_google),
                loading = state.googleSigningIn,
                enabled = activity != null && !busy,
                onClick = { activity?.let { vm.signInWithGoogle(it, onSignedIn) } },
            )
            Spacer(modifier = Modifier.height(12.dp))
            SocialButton(
                label = stringResource(R.string.auth_continue_with_apple),
                loading = state.appleSigningIn,
                enabled = activity != null && !busy,
                onClick = { activity?.let { vm.signInWithApple(it, onSignedIn) } },
            )
            Spacer(modifier = Modifier.height(12.dp))
            SocialButton(
                label = stringResource(R.string.auth_continue_with_wechat),
                loading = state.wechatSigningIn,
                enabled = !busy,
                onClick = { vm.signInWithWeChat(onSignedIn) },
            )
            Spacer(modifier = Modifier.height(12.dp))
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

@Composable
internal fun BackendOnlyMenu(currentLanguage: String) {
    var expanded by remember { mutableStateOf(false) }
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
                text = { Text(stringResource(R.string.chat_backend)) },
                onClick = {
                    expanded = false
                    showBackendDialog = true
                },
            )
        }
    }
    if (showBackendDialog) {
        BaseUrlDialog(
            currentLanguage = currentLanguage,
            onDismiss = { showBackendDialog = false },
        )
    }
}
