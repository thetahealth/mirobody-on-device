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
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DrawerDefaults
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.outlined.Menu
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.ContentMaxWidth
import ai.thetahealth.mirobody.ui.DrawerDivider
import ai.thetahealth.mirobody.ui.DrawerHeader
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.LocalLayoutInfo
import ai.thetahealth.mirobody.ui.settings.AppSettingsSection
import kotlinx.coroutines.launch

/**
 * Single-screen sign-in, mirroring the web client (`htdoc/src/login.js`): a
 * centered brand block, the configured social providers, an OR divider, then
 * email + one-time code (a code field with an inline Send code and a primary
 * Sign in) — no separate verify screen.
 *
 * The email form is the same staircase as login.js, each step unlocking the next:
 * a valid-looking address unlocks Send code; a successful send unlocks the code field
 * (editing the address re-locks it until a code is sent to that one); six digits
 * unlock Sign in, and also submit on their own. Send code has two states that own the
 * button outright — the request in flight, and the resend cooldown after it. One
 * status line under the form narrates all of it, federated sign-in included.
 *
 * [onCancel] is set only when this screen was pushed over a live session to add
 * another account. It decides the top bar's left affordance, exactly as the web
 * client's `leftMode` does: a back arrow to the current account when adding one (a
 * drawer would be a dead end there), otherwise the hamburger that opens the nav
 * drawer — narrowed to the app-settings group, since everything else in it is
 * session-scoped.
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun EmailScreen(
    onSignedIn: () -> Unit,
    onCancel: (() -> Unit)? = null,
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
    // Server-advertised provider availability (GET /auth/providers). Adaptive
    // sign-in: only show a federated button the server actually configured —
    // mirrors the web client and avoids e.g. a WeChat button on a server without
    // WeChat. Null until the fetch lands, so every button starts hidden.
    val serverConfig by container.serverConfigStore.config.collectAsState()
    // Resolve the Activity from the hosting ComposeView's context (needed to launch
    // the federated sign-in flows).
    val activity = LocalView.current.context.findActivity()

    val codeFocus = remember { FocusRequester() }
    // Move to the code field as soon as a code is on its way to the address in the
    // field -- keyed on the address, so a send to a NEW one moves the caret again.
    LaunchedEffect(state.codeSentTo) {
        if (state.codeSentTo.isNotEmpty()) codeFocus.requestFocus()
    }
    // Six digits submit on their own; the same gate the Sign in button uses, so an
    // auto-submit can never fire on a code the button would refuse. A rejected code
    // stays in the field unchanged, which is what stops this from re-firing.
    LaunchedEffect(state.code) { if (state.canVerify) vm.verify(onSignedIn) }

    val drawerState = rememberDrawerState(initialValue = DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    val layout = LocalLayoutInfo.current

    ModalNavigationDrawer(
        drawerState = drawerState,
        // Adding an account has a back arrow instead of a hamburger, so there is no
        // way to open this drawer then -- gesture included, or a swipe from the edge
        // would summon a menu the screen doesn't advertise.
        gesturesEnabled = onCancel == null,
        drawerContent = {
            ModalDrawerSheet(
                modifier = Modifier
                    .fillMaxWidth(layout.drawerWidthFraction)
                    .widthIn(max = layout.drawerMaxWidth)
                    .shadow(elevation = 8.dp, shape = DrawerDefaults.shape),
            ) {
                // Signed out, the drawer is the app-settings group and nothing else:
                // history, New chat / Incognito, health connections and the account
                // rows are all session-scoped. Scrollable all the same, so a short
                // sheet (landscape, or a large font size) can reach every row -- the
                // chat drawer scrolls for the same reason.
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .verticalScroll(rememberScrollState()),
                ) {
                    DrawerHeader(onClose = { scope.launch { drawerState.close() } })
                    DrawerDivider()
                    AppSettingsSection(currentLanguage = language)
                    Spacer(modifier = Modifier.height(6.dp))
                }
            }
        },
    ) {
    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            CenterAlignedTopAppBar(
                colors = TopAppBarDefaults.centerAlignedTopAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background,
                ),
                // No wordmark in the bar on this screen: the card below is already the
                // brand statement (mark + display-size "Mirobody"), and a second one
                // 56dp above it would state the brand twice on one screen.
                title = {},
                navigationIcon = {
                    if (onCancel != null) {
                        IconButton(onClick = onCancel) {
                            Icon(
                                Icons.AutoMirrored.Outlined.ArrowBack,
                                contentDescription = stringResource(R.string.common_back),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    } else {
                        IconButton(onClick = { scope.launch { drawerState.open() } }) {
                            Icon(
                                Icons.Outlined.Menu,
                                contentDescription = stringResource(R.string.chat_menu_title),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                },
                // No right-hand actions: the settings gear that used to sit here is a
                // group inside the drawer now, the same as on the chat screen.
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
            // Every provider is server-gated now, so the row can come out empty --
            // on a server with none configured, or before the capability document
            // lands. Skip the row and the OR divider together when it would: a lone
            // "or" with nothing above it reads as a rendering fault.
            val anySocial = serverConfig?.firebaseVerifyEnabled == true ||
                serverConfig?.wechatEnabled == true
            if (anySocial) {
            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(12.dp, Alignment.CenterHorizontally),
                verticalArrangement = Arrangement.spacedBy(12.dp),
                modifier = Modifier.fillMaxWidth(),
            ) {
                // Show a federated icon only when the server can actually complete
                // that sign-in (GET /auth/providers). The gate is the VERIFICATION
                // PATH, not the provider name: this client brokers Google, Apple,
                // GitHub and X alike through Firebase and POSTs to /firebase/verify,
                // so all four hang off firebaseVerifyEnabled. GITHUB_CLIENT_ID and
                // APPLE_CLIENT_ID gate the *browser*, which does its own server-side
                // exchange -- reading them here would hide working buttons.
                if (serverConfig?.firebaseVerifyEnabled == true) {
                    SocialIconButton(
                        iconRes = R.drawable.ic_provider_google,
                        contentDescription = stringResource(R.string.auth_continue_with_google),
                        tint = null,   // official four-color "G" — keep its own fills
                        loading = state.googleSigningIn,
                        enabled = activity != null && !busy,
                        onClick = { activity?.let { vm.signInWithGoogle(it, onSignedIn) } },
                    )
                }
                if (serverConfig?.firebaseVerifyEnabled == true) {
                    SocialIconButton(
                        iconRes = R.drawable.ic_provider_apple,
                        contentDescription = stringResource(R.string.auth_continue_with_apple),
                        tint = markTint,
                        loading = state.appleSigningIn,
                        enabled = activity != null && !busy,
                        onClick = { activity?.let { vm.signInWithApple(it, onSignedIn) } },
                    )
                }
                if (serverConfig?.wechatEnabled == true) {
                    SocialIconButton(
                        iconRes = R.drawable.ic_provider_wechat,
                        contentDescription = stringResource(R.string.auth_continue_with_wechat),
                        tint = Color(0xFF07C160),   // WeChat green, fixed on both themes
                        loading = state.wechatSigningIn,
                        enabled = !busy,
                        onClick = { vm.signInWithWeChat(onSignedIn) },
                    )
                }
                if (serverConfig?.firebaseVerifyEnabled == true) {
                    SocialIconButton(
                        iconRes = R.drawable.ic_provider_github,
                        contentDescription = stringResource(R.string.auth_continue_with_github),
                        tint = markTint,
                        loading = state.githubSigningIn,
                        enabled = activity != null && !busy,
                        onClick = { activity?.let { vm.signInWithGithub(it, onSignedIn) } },
                    )
                }
                if (serverConfig?.firebaseVerifyEnabled == true) {
                    SocialIconButton(
                        iconRes = R.drawable.ic_provider_x,
                        contentDescription = stringResource(R.string.auth_continue_with_x),
                        tint = markTint,
                        loading = state.xSigningIn,
                        enabled = activity != null && !busy,
                        onClick = { activity?.let { vm.signInWithX(it, onSignedIn) } },
                    )
                }
            }

            // -- OR divider (only meaningful with a social row above it) ------
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
            }   // end anySocial
            Spacer(modifier = Modifier.height(20.dp))

            // -- email + one-time code ------------------------------------------
            // The address: valid shape unlocks Send code, and the IME's action key
            // sends too (login.js binds Enter here to the same button). No error tint
            // -- the status line below carries every message, as on the web.
            OutlinedTextField(
                value = state.email,
                onValueChange = vm::onEmailChange,
                label = { Text(stringResource(R.string.email_label)) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(
                    keyboardType = KeyboardType.Email,
                    imeAction = ImeAction.Send,
                ),
                keyboardActions = KeyboardActions(onSend = { if (state.canSendCode) vm.sendCode() }),
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
                // The code: locked until a code has actually gone to the address in
                // the field, tinted while the last one stands rejected, and the IME's
                // action key signs in once six digits are there.
                OutlinedTextField(
                    value = state.code,
                    onValueChange = vm::onCodeChange,
                    label = { Text(stringResource(R.string.verification_code)) },
                    singleLine = true,
                    enabled = state.codeSent,
                    isError = state.codeRejected,
                    keyboardOptions = KeyboardOptions(
                        keyboardType = KeyboardType.NumberPassword,
                        imeAction = ImeAction.Done,
                    ),
                    keyboardActions = KeyboardActions(onDone = { if (state.canVerify) vm.verify(onSignedIn) }),
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
                    enabled = state.canSendCode,
                    modifier = Modifier.height(56.dp),
                    shape = RoundedCornerShape(10.dp),
                ) {
                    when {
                        // A request in flight and the cooldown after it each own the
                        // button; once a code has been sent, it reads "Resend code".
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
                        state.codeSentTo.isNotEmpty() -> {
                            Text(stringResource(R.string.verify_resend_code))
                        }
                        else -> {
                            Text(stringResource(R.string.email_send_code))
                        }
                    }
                }
            }
            Spacer(modifier = Modifier.height(16.dp))
            // Sign in needs the whole staircase: a valid address, a code sent to it,
            // and all six digits.
            Button(
                onClick = { vm.verify(onSignedIn) },
                enabled = state.canVerify,
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

            // The one status line every flow writes to (login.js's `status`): progress
            // and confirmations in the muted ink, failures in the error ink — the
            // server's own message when it sent one, else the flow's fallback.
            Spacer(modifier = Modifier.height(8.dp))
            val status = state.status
            val statusText = when (status) {
                is EmailStatus.None -> ""
                is EmailStatus.Info ->
                    if (status.arg != null) stringResource(status.res, status.arg)
                    else stringResource(status.res)
                is EmailStatus.Error -> status.message ?: stringResource(status.res)
            }
            Text(
                text = statusText,
                style = MaterialTheme.typography.bodySmall,
                color = if (status is EmailStatus.Error) MaterialTheme.colorScheme.error
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

// App settings live in ui/settings/AppSettingsSection.kt, rendered as drawer rows —
// this screen's drawer shows that group alone, the chat drawer shows it among the
// session-scoped ones. One definition, so the two screens can't drift.
