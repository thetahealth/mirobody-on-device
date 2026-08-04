package ai.thetahealth.mirobody.ui.chat

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.ArrowDropDown
import androidx.compose.material.icons.outlined.AttachFile
import androidx.compose.material.icons.outlined.AutoFixHigh
import androidx.compose.material.icons.outlined.Build
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material.icons.outlined.ExpandLess
import androidx.compose.material.icons.outlined.ExpandMore
import androidx.compose.material.icons.outlined.Info
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.Menu
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DrawerDefaults
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ai.thetahealth.mirobody.R
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.data.chat.dto.ChatAttachment
import ai.thetahealth.mirobody.data.chat.dto.CostStatistics
import ai.thetahealth.mirobody.data.chat.dto.ProviderInfo
import ai.thetahealth.mirobody.data.llm.MlKitTextService
import ai.thetahealth.mirobody.data.llm.OnDeviceModel
import ai.thetahealth.mirobody.data.llm.OnDeviceModelSpec
import ai.thetahealth.mirobody.data.llm.OnDeviceModelStatus
import ai.thetahealth.mirobody.data.circle.dto.HealthSharer
import ai.thetahealth.mirobody.data.settings.StoredAccount
import ai.thetahealth.mirobody.ui.DrawerDivider
import ai.thetahealth.mirobody.ui.DrawerHeader
import ai.thetahealth.mirobody.ui.DrawerRow
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.LocalLayoutInfo
import ai.thetahealth.mirobody.ui.circle.CareCircleDialog
import ai.thetahealth.mirobody.ui.circle.ShareConversationDialog
import ai.thetahealth.mirobody.ui.health.BleDeviceDialog
import ai.thetahealth.mirobody.ui.health.EhrDialog
import ai.thetahealth.mirobody.ui.health.HdpDeviceDialog
import ai.thetahealth.mirobody.ui.health.HealthSyncDialog
import ai.thetahealth.mirobody.ui.settings.AppSettingsSection
import ai.thetahealth.mirobody.ui.vendor.VendorsDialog
import ai.thetahealth.mirobody.ui.theme.BrandBlue
import ai.thetahealth.mirobody.ui.theme.MirobodyTheme
import coil.compose.AsyncImage
import coil.request.ImageRequest
import android.content.Context
import android.content.res.Configuration
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.OpenableColumns
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(
    // Recreate the chat for the now-current account (after a switch, or after
    // signing out to a remaining account).
    onRelaunch: () -> Unit,
    // Show the login screen over this session to add another account.
    onAddAccount: () -> Unit,
) {
    val container = LocalAppContainer.current
    val vm: ChatViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                ChatViewModel(
                    container.chatRepository,
                    container.circleRepository,
                    container.settings,
                    container.errorBus,
                    container.chatHistoryStore,
                    container.modelManager,
                    container.mlKitTextService,
                )
            }
        },
    )
    val layout = LocalLayoutInfo.current
    val state by vm.state.collectAsState()
    val listState = rememberLazyListState()
    val scope = rememberCoroutineScope()
    val drawerState = rememberDrawerState(initialValue = DrawerValue.Closed)
    val context = LocalContext.current
    val currentEmail by container.settings.currentEmail.collectAsState(initial = null)
    // Sign out the CURRENT account. Clear its local chat first (the key is derived
    // from the still-current sub), then drop its token. If another account remains,
    // recreate the chat as it; otherwise the token goes null and the nav graph's
    // token collector returns to login.
    val doSignOut: () -> Unit = {
        scope.launch {
            container.chatHistoryStore.clear()
            container.authRepository.signOut()
            if (container.settings.hasAccount()) onRelaunch()
        }
    }
    // Switch to another stored account, then recreate the chat as it.
    val onSwitchAccount: (String) -> Unit = { sub ->
        scope.launch {
            container.settings.switchAccount(sub)
            onRelaunch()
        }
    }

    // System file picker → read each pick into a ChatAttachment off the main thread,
    // then stage it in the composer (mirrors the web client's paperclip upload).
    val filePicker = rememberLauncherForActivityResult(
        ActivityResultContracts.GetMultipleContents(),
    ) { uris ->
        if (uris.isEmpty()) return@rememberLauncherForActivityResult
        scope.launch {
            val atts = withContext(Dispatchers.IO) { uris.mapNotNull { readChatAttachment(context, it) } }
            atts.forEach(vm::addAttachment)
        }
    }

    // Keep the current output line (the bottom of the streaming message) in view as it
    // grows, but never fight the user: a manual scroll up detaches the follow so they
    // can re-read earlier content mid-reply. Scrolling back down to the output line
    // (i.e. the very bottom), or starting a new turn, re-arms the follow.
    //
    // We must reveal the *bottom* of the last message, not its top: when a reply is
    // taller than the screen, aligning the item's top would leave the streaming tail
    // off-screen below. So scroll the item into view, then scroll the overshoot so its
    // bottom edge sits at the viewport bottom.
    var followTail by remember { mutableStateOf(true) }
    suspend fun scrollToOutput() {
        val lastIndex = state.messages.lastIndex
        if (lastIndex < 0) return
        val info = listState.layoutInfo
        val visible = info.visibleItemsInfo.lastOrNull { it.index == lastIndex }
        if (visible == null) {
            // Last message is off-screen (e.g. resuming after scrolling far up): bring
            // it into view first, then fall through to reveal its bottom edge.
            listState.scrollToItem(lastIndex)
        }
        val laid = listState.layoutInfo
        val item = laid.visibleItemsInfo.lastOrNull { it.index == lastIndex } ?: return
        val overshoot = item.offset + item.size - laid.viewportEndOffset
        if (overshoot > 0) listState.scrollBy(overshoot.toFloat())
    }

    // A manual upward scroll detaches the follow; reaching the bottom (the output line)
    // re-arms it. Content growth pushes the bottom farther away, never toward it, so
    // it can only detach — reaching the bottom is always a deliberate user scroll.
    LaunchedEffect(listState) {
        var prevIndex = listState.firstVisibleItemIndex
        var prevOffset = listState.firstVisibleItemScrollOffset
        snapshotFlow { listState.firstVisibleItemIndex to listState.firstVisibleItemScrollOffset }
            .collect { (index, offset) ->
                val scrolledUp = index < prevIndex || (index == prevIndex && offset < prevOffset)
                if (scrolledUp) followTail = false
                else if (!listState.canScrollForward) followTail = true
                prevIndex = index
                prevOffset = offset
            }
    }
    // A new turn (user just sent, or the assistant bubble just appeared) always snaps
    // to the output line and re-arms tail-follow.
    LaunchedEffect(state.messages.size) {
        if (state.messages.isNotEmpty()) {
            followTail = true
            scrollToOutput()
        }
    }
    // The streaming assistant turn grows through several fields — the reply `text`, the
    // `thinking` trace, tool-call cards, images, charts — so follow the output on ANY
    // change to the last message, not just `text`. (Keying on `text.length` alone
    // missed the thinking phase: a long thinking trace grew past the screen bottom
    // without the list following it.)
    LaunchedEffect(state.messages.lastOrNull()) {
        if (state.messages.isNotEmpty() && followTail) scrollToOutput()
    }

    // On-device model manager dialog. Opens when the user picks the "manage" entry;
    // downloaded models are selected directly like any other provider.
    var showModelDialog by remember { mutableStateOf(false) }
    val onProviderSelected: (ProviderInfo) -> Unit = { provider ->
        if (provider.isManageEntry) {
            showModelDialog = true
        } else {
            vm.onProviderSelected(provider)
        }
    }
    if (showModelDialog) {
        OnDeviceModelDialog(
            statuses = state.onDeviceModels,
            imported = state.onDeviceImported,
            hasStorageAccess = vm::hasStorageAccess,
            onDownload = vm::downloadOnDeviceModel,
            onDelete = vm::deleteOnDeviceModel,
            onImport = vm::importOnDeviceModel,
            onDeleteImported = vm::deleteImportedOnDeviceModel,
            onDismiss = { showModelDialog = false },
        )
    }

    ModalNavigationDrawer(
        drawerState = drawerState,
        drawerContent = {
            ModalDrawerSheet(
                modifier = Modifier
                    .fillMaxWidth(layout.drawerWidthFraction)
                    .widthIn(max = layout.drawerMaxWidth)
                    .shadow(elevation = 8.dp, shape = DrawerDefaults.shape),
            ) {
                ChatDrawer(
                    conversationId = state.conversationId,
                    isActive = drawerState.targetValue == DrawerValue.Open,
                    incognito = state.incognito,
                    onToggleIncognito = {
                        vm.toggleIncognito()
                        scope.launch { drawerState.close() }
                    },
                    onNewChat = {
                        vm.newChat()
                        scope.launch { drawerState.close() }
                    },
                    onOpenConversation = { id ->
                        vm.openConversation(id)
                        scope.launch { drawerState.close() }
                    },
                    onClose = { scope.launch { drawerState.close() } },
                    currentEmail = currentEmail,
                    language = state.language,
                    onSwitchAccount = { sub ->
                        onSwitchAccount(sub)
                        scope.launch { drawerState.close() }
                    },
                    onAddAccount = {
                        scope.launch { drawerState.close() }
                        onAddAccount()
                    },
                    onSignOut = doSignOut,
                )
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
                // Left zone: the hamburger, and the wordmark beside it -- the same
                // arrangement as the web bar. It was an account avatar before; a nav
                // glyph says more about what the drawer holds (the conversation list
                // is its body, account is one pinned row at the bottom).
                navigationIcon = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        IconButton(onClick = { scope.launch { drawerState.open() } }) {
                            Icon(
                                Icons.Outlined.Menu,
                                contentDescription = stringResource(R.string.chat_menu_title),
                                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        // The wordmark and the subject picker are alternatives for one
                        // row: without a picker the bar would otherwise be blank, and
                        // with one there isn't width for both. Nothing has shared health
                        // data -> brand; something has -> the picker takes the centre.
                        if (state.sharers.isEmpty()) {
                            Wordmark()
                        }
                    }
                },
                // Centre: only what the view puts there -- the subject picker, which on
                // the web moves up here on a narrow screen rather than sitting in the
                // composer. There are NO right-hand actions: the settings gear that used
                // to fill that slot is a group inside the drawer now, so the bar's one
                // affordance is the hamburger.
                title = {
                    if (state.sharers.isNotEmpty()) {
                        SubjectPicker(
                            sharers = state.sharers,
                            subject = state.subject,
                            onSelect = vm::onSubjectSelected,
                        )
                    }
                },
            )
        },
        bottomBar = {
            Surface(
                color = MaterialTheme.colorScheme.background,
                modifier = Modifier
                    .imePadding()
                    .navigationBarsPadding(),
            ) {
                Box(
                    modifier = Modifier.fillMaxWidth(),
                    contentAlignment = Alignment.Center,
                ) {
                    Column(
                        modifier = Modifier
                            .widthIn(max = layout.contentMaxWidth)
                            .padding(horizontal = 12.dp, vertical = 8.dp),
                    ) {
                        // (The subject picker used to sit here; it lives in the top bar's
                        // centre slot now -- see the app bar above.)
                        // Staged attachments, removable until the turn is sent.
                        if (state.attachments.isNotEmpty()) {
                            AttachmentChips(
                                attachments = state.attachments,
                                onRemove = vm::removeAttachment,
                            )
                        }
                        val canSend = !state.sending && state.selected != null &&
                            (state.input.isNotBlank() || state.attachments.isNotEmpty())
                        val doSend: () -> Unit = {
                            // The "manage" entry isn't a chat provider — open the manager
                            // instead of sending. Downloaded models send normally.
                            if (state.selected?.isManageEntry == true) {
                                showModelDialog = true
                            } else {
                                vm.send()
                            }
                        }
                        // One rounded pill: the input on top, a control row (attach ·
                        // model picker · send) below — matching the web composer.
                        Surface(
                            shape = RoundedCornerShape(24.dp),
                            color = MaterialTheme.colorScheme.surfaceContainerLow,
                            border = androidx.compose.foundation.BorderStroke(
                                1.dp, MaterialTheme.colorScheme.outlineVariant,
                            ),
                        ) {
                            Column(modifier = Modifier.padding(start = 8.dp, end = 8.dp, top = 6.dp, bottom = 6.dp)) {
                                ChatInputField(
                                    value = state.input,
                                    onValueChange = vm::onInputChange,
                                    enabled = !state.sending,
                                    modifier = Modifier.fillMaxWidth(),
                                )
                                Row(
                                    verticalAlignment = Alignment.CenterVertically,
                                    modifier = Modifier.fillMaxWidth(),
                                ) {
                                    IconButton(
                                        onClick = { filePicker.launch("*/*") },
                                        enabled = !state.sending,
                                        modifier = Modifier.size(40.dp),
                                    ) {
                                        Icon(
                                            imageVector = Icons.Outlined.AttachFile,
                                            contentDescription = stringResource(R.string.chat_attach_file),
                                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                        )
                                    }
                                    // On-device draft rewrite (Gemini Nano) — only where supported.
                                    if (state.polishAvailable) {
                                        PolishButton(
                                            enabled = !state.sending && state.input.isNotBlank(),
                                            polishing = state.polishing,
                                            onPolish = vm::polishDraft,
                                        )
                                    }
                                    // Model picker takes the middle, centered.
                                    ProviderMenu(
                                        state = state,
                                        onSelect = onProviderSelected,
                                        onRetry = vm::loadProviders,
                                        modifier = Modifier.weight(1f),
                                    )
                                    SendButton(
                                        enabled = canSend,
                                        sending = state.sending,
                                        onClick = doSend,
                                    )
                                }
                            }
                        }
                    }
                }
            }
        },
    ) { padding ->
        Column(modifier = Modifier.fillMaxSize().padding(padding)) {
            if (state.messages.isEmpty()) {
                EmptyState(incognito = state.incognito, modifier = Modifier.weight(1f))
            } else {
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    contentAlignment = Alignment.TopCenter,
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .widthIn(max = layout.contentMaxWidth)
                            .padding(horizontal = layout.screenPadding),
                    ) {
                        // Slim banner while an incognito session already has messages
                        // (the empty state carries its own hero instead).
                        if (state.incognito) {
                            IncognitoBanner()
                        }
                        LazyColumn(
                            state = listState,
                            modifier = Modifier.fillMaxSize(),
                            contentPadding = PaddingValues(vertical = layout.screenPadding),
                            verticalArrangement = Arrangement.spacedBy(layout.messageSpacing),
                        ) {
                            items(state.messages, key = { it.id }) { msg ->
                                MessageBubble(msg)
                            }
                        }
                    }
                }
            }
        }
    }
    }
}

/**
 * The brand in the top app bar: the serif wordmark alone, sitting next to the
 * hamburger. No logo mark here — the sign-in card is the screen that states the brand
 * in full (mark + display-size wordmark), and repeating the mark in a 56dp bar reads
 * as decoration rather than identity.
 */
@Composable
private fun Wordmark() {
    Text(
        text = stringResource(R.string.app_name),
        style = MaterialTheme.typography.titleLarge.copy(
            fontFamily = FontFamily.Serif,
            fontWeight = FontWeight.SemiBold,
        ),
        color = MaterialTheme.colorScheme.onSurface,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier.padding(start = 4.dp),
    )
}

@Composable
private fun EmptyState(incognito: Boolean, modifier: Modifier = Modifier) {
    Box(modifier = modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(if (incognito) 20.dp else 6.dp),
            modifier = Modifier.padding(24.dp),
        ) {
            if (incognito) {
                // Privacy hero: the big ghost + "You're incognito" + the not-saved note.
                Icon(
                    painter = androidx.compose.ui.res.painterResource(R.drawable.ic_incognito),
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.size(56.dp),
                )
                Text(
                    text = stringResource(R.string.chat_incognito_heading),
                    style = MaterialTheme.typography.headlineSmall,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    text = stringResource(R.string.chat_incognito_note),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                )
            } else {
                Text(
                    text = stringResource(R.string.chat_empty_title),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Text(
                    text = stringResource(R.string.chat_empty_subtitle),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

/** Slim pill pinned above an incognito thread, reminding it won't be saved. */
@Composable
private fun IncognitoBanner() {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 8.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(MaterialTheme.colorScheme.surfaceContainerLow)
            .padding(horizontal = 12.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            painter = androidx.compose.ui.res.painterResource(R.drawable.ic_incognito),
            contentDescription = null,
            tint = MaterialTheme.colorScheme.primary,
            modifier = Modifier.size(16.dp),
        )
        Text(
            text = stringResource(R.string.chat_incognito_note),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 8.dp),
        )
    }
}

/** Borderless, auto-growing message field (the pill around it owns the outline). */
@Composable
private fun ChatInputField(
    value: String,
    onValueChange: (String) -> Unit,
    enabled: Boolean,
    modifier: Modifier = Modifier,
) {
    val hint = stringResource(R.string.chat_message_hint)
    val hintColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
    Box(modifier = modifier.padding(horizontal = 8.dp, vertical = 6.dp)) {
        if (value.isEmpty()) {
            Text(hint, color = hintColor, style = MaterialTheme.typography.bodyLarge)
        }
        BasicTextField(
            value = value,
            onValueChange = onValueChange,
            modifier = Modifier.fillMaxWidth(),
            enabled = enabled,
            maxLines = 6,
            textStyle = MaterialTheme.typography.bodyLarge.copy(color = MaterialTheme.colorScheme.onSurface),
            cursorBrush = SolidColor(MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)),
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Default),
        )
    }
}

/** Circular navy send button (the composer's primary action). Shows a spinner
 *  while a reply is streaming; the up-arrow otherwise. */
@Composable
private fun SendButton(
    enabled: Boolean,
    sending: Boolean,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .size(40.dp)
            .clip(CircleShape)
            .background(
                if (enabled) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.primary.copy(alpha = 0.4f),
            )
            .clickable(enabled = enabled && !sending, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        if (sending) {
            CircularProgressIndicator(
                modifier = Modifier.size(20.dp),
                strokeWidth = 2.dp,
                color = MaterialTheme.colorScheme.onPrimary,
            )
        } else {
            Icon(
                painter = androidx.compose.ui.res.painterResource(R.drawable.ic_send_arrow),
                contentDescription = stringResource(R.string.chat_send),
                tint = MaterialTheme.colorScheme.onPrimary,
                modifier = Modifier.size(22.dp),
            )
        }
    }
}

/** Label for a sharer in the subject picker: nickname, else email, else "#handle". */
private fun HealthSharer.displayLabel(): String =
    nickname.ifBlank { email }.ifBlank { "#$member" }

/**
 * Whose health the turn is about — shown in the top bar's centre slot, and only when
 * care-circle members have actually shared data with this user. Picking one sends
 * `subject` so the AI's family_health tool defaults to that member ("how is Mom
 * doing?").
 *
 * No "Currently for" label: the web client dropped it, and in a 56dp bar the label
 * would eat the width the names need. The dropdown carries the meaning on its own —
 * its default option is "Me".
 */
@Composable
private fun SubjectPicker(
    sharers: List<HealthSharer>,
    subject: Long,
    onSelect: (Long) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    val meLabel = stringResource(R.string.chat_subject_me)
    val selectedLabel = if (subject == 0L) meLabel
        else sharers.firstOrNull { it.member == subject }?.displayLabel() ?: meLabel
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box {
            TextButton(
                onClick = { expanded = true },
                shape = RoundedCornerShape(10.dp),
            ) {
                Text(
                    text = selectedLabel,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Icon(
                    Icons.Outlined.ArrowDropDown,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                DropdownMenuItem(
                    text = { Text(meLabel) },
                    onClick = {
                        onSelect(0)
                        expanded = false
                    },
                )
                sharers.forEach { sharer ->
                    DropdownMenuItem(
                        text = { Text(sharer.displayLabel()) },
                        onClick = {
                            onSelect(sharer.member)
                            expanded = false
                        },
                    )
                }
            }
        }
    }
}

/** Read a picked content Uri into an in-memory attachment (display name, mime, bytes). */
private fun readChatAttachment(context: Context, uri: Uri): ChatAttachment? = runCatching {
    val resolver = context.contentResolver
    val mime = resolver.getType(uri) ?: "application/octet-stream"
    val name = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
        ?.use { c -> if (c.moveToFirst() && !c.isNull(0)) c.getString(0) else null }
        ?: uri.lastPathSegment ?: "file"
    val bytes = resolver.openInputStream(uri)?.use { it.readBytes() }
    if (bytes == null) null else ChatAttachment(fileName = name, mimeType = mime, bytes = bytes)
}.getOrNull()

@Composable
private fun AttachmentChips(
    attachments: List<ChatAttachment>,
    onRemove: (Int) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp, vertical = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        attachments.forEachIndexed { index, att ->
            Surface(
                shape = RoundedCornerShape(10.dp),
                color = MaterialTheme.colorScheme.surfaceContainerHigh,
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.padding(start = 10.dp, end = 2.dp, top = 2.dp, bottom = 2.dp),
                ) {
                    Icon(
                        imageVector = Icons.Outlined.Description,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(16.dp),
                    )
                    Text(
                        text = att.fileName,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 1,
                        modifier = Modifier
                            .padding(start = 6.dp)
                            .widthIn(max = 160.dp),
                    )
                    IconButton(onClick = { onRemove(index) }, modifier = Modifier.size(28.dp)) {
                        Icon(
                            imageVector = Icons.Outlined.Close,
                            contentDescription = stringResource(R.string.chat_attach_remove),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.size(14.dp),
                        )
                    }
                }
            }
        }
    }
}

/**
 * Left navigation drawer, the app's only menu (mirrors `htdoc/src/history.js`): a
 * header with the "Menu" title, a "New chat" / "Incognito" button row, the
 * conversation history, then three groups — health & data, app settings, and the
 * account rows.
 *
 * Everything under the header is one scrolling list. `htdoc` pins those three groups
 * to the bottom and scrolls only the history, but they come to ~585dp of rows: pinned,
 * they overflowed a phone sheet in landscape (and at a larger font size, and on short
 * screens) with no way to scroll to what fell off.
 *
 * The app-settings group used to be a top-right gear on both this screen and login.
 * Folding it in here leaves one menu affordance instead of two, and lets the login
 * screen open the same drawer narrowed to just that group.
 */
@Composable
private fun ChatDrawer(
    conversationId: String,
    isActive: Boolean,
    incognito: Boolean,
    currentEmail: String?,
    language: String,
    onToggleIncognito: () -> Unit,
    onNewChat: () -> Unit,
    onOpenConversation: (String) -> Unit,
    onClose: () -> Unit,
    onSwitchAccount: (String) -> Unit,
    onAddAccount: () -> Unit,
    onSignOut: () -> Unit,
) {
    val container = LocalAppContainer.current
    val accounts by container.settings.accounts.collectAsState(initial = emptyList())
    var switcherOpen by remember { mutableStateOf(false) }
    var showHealthDialog by remember { mutableStateOf(false) }
    var showBleDialog by remember { mutableStateOf(false) }
    var showHdpDialog by remember { mutableStateOf(false) }
    var showCircleDialog by remember { mutableStateOf(false) }
    var showVendorsDialog by remember { mutableStateOf(false) }
    var showEhrDialog by remember { mutableStateOf(false) }
    var showShareDialog by remember { mutableStateOf(false) }
    var showSignOutDialog by remember { mutableStateOf(false) }

    val history = rememberHistoryController(isActive = isActive)

        Column(modifier = Modifier.fillMaxSize()) {
            // Only the header is pinned -- it holds the affordance that closes the
            // drawer, so it must stay in reach. Everything below scrolls as ONE region:
            // the menu groups alone are ~585dp tall, so when they were pinned to the
            // bottom (with history the only scroller) they overflowed the sheet on a
            // short screen, in landscape, or at a larger font size, and the rows past
            // the fold could not be reached at all.
            DrawerHeader(onClose = onClose)
            LazyColumn(modifier = Modifier.fillMaxWidth().weight(1f)) {
                // New chat + Incognito: two equal-width bordered text buttons in one row.
                item("actions") {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 16.dp, vertical = 12.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        DrawerActionButton(
                            label = stringResource(R.string.chat_new_chat),
                            active = false,
                            onClick = onNewChat,
                            modifier = Modifier.weight(1f),
                        )
                        DrawerActionButton(
                            label = stringResource(R.string.chat_incognito_mode),
                            active = incognito,
                            onClick = onToggleIncognito,
                            modifier = Modifier.weight(1f),
                        )
                    }
                }
                // The conversation list. (No divider before it -- each history row
                // carries its own top rule.)
                historySection(history, onOpen = onOpenConversation)
                // The menu groups, below the history rather than pinned under them.
                item("menu") {
                    Column(modifier = Modifier.fillMaxWidth()) {
                        DrawerDivider()
                        // Health & data.
                        DrawerRow(stringResource(R.string.chat_care_circle), onClick = { showCircleDialog = true })
                        DrawerRow(stringResource(R.string.chat_vendors), onClick = { showVendorsDialog = true })
                        DrawerRow(stringResource(R.string.chat_ehr), onClick = { showEhrDialog = true })
                        DrawerRow(stringResource(R.string.chat_sync_health), onClick = { showHealthDialog = true })
                        DrawerRow(stringResource(R.string.chat_bluetooth), onClick = { showBleDialog = true })
                        // Legacy classic-Bluetooth HDP only runs on Android 9 and below.
                        if (Build.VERSION.SDK_INT <= 28) {
                            DrawerRow(stringResource(R.string.chat_bluetooth_hdp), onClick = { showHdpDialog = true })
                        }
                        if (conversationId.isNotBlank()) {
                            DrawerRow(stringResource(R.string.chat_share), onClick = { showShareDialog = true })
                        }
                        DrawerDivider()
                        // App settings (language / font / backend) -- what the
                        // top-bar gear used to hold. The same group the login drawer
                        // shows on its own.
                        AppSettingsSection(currentLanguage = language)
                        DrawerDivider()
                        // Account switcher: the current email expands the other signed-in
                        // accounts (tap to switch) plus "Add account"; Sign out is below.
                        val others = accounts.filter { !it.current }
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { switcherOpen = !switcherOpen }
                                .padding(horizontal = 20.dp, vertical = 12.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                text = currentEmail?.takeIf { it.isNotBlank() }
                                    ?: stringResource(R.string.chat_account),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier.weight(1f),
                            )
                            Icon(
                                if (switcherOpen) Icons.Outlined.ExpandLess else Icons.Outlined.ExpandMore,
                                contentDescription = null,
                                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        if (switcherOpen) {
                            others.forEach { acc ->
                                DrawerRow(
                                    label = acc.email.ifBlank { "#" + acc.sub.take(6) },
                                    onClick = { onSwitchAccount(acc.sub) },
                                )
                            }
                            DrawerRow(
                                label = stringResource(R.string.chat_add_account),
                                onClick = onAddAccount,
                                leading = {
                                    Icon(
                                        Icons.Outlined.Add,
                                        contentDescription = null,
                                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                },
                            )
                        }
                        DrawerRow(
                            label = stringResource(R.string.chat_sign_out),
                            onClick = { showSignOutDialog = true },
                            danger = true,
                            center = true,
                        )
                        Spacer(Modifier.height(6.dp))
                    }
                }
            }
        }

    if (showHealthDialog) {
        HealthSyncDialog(
            onDismiss = { showHealthDialog = false },
        )
    }

    if (showBleDialog) {
        BleDeviceDialog(
            onDismiss = { showBleDialog = false },
        )
    }

    if (showHdpDialog) {
        HdpDeviceDialog(
            onDismiss = { showHdpDialog = false },
        )
    }

    if (showVendorsDialog) {
        VendorsDialog(
            onDismiss = { showVendorsDialog = false },
        )
    }

    if (showEhrDialog) {
        EhrDialog(onDismiss = { showEhrDialog = false })
    }

    if (showCircleDialog) {
        CareCircleDialog(
            onDismiss = { showCircleDialog = false },
        )
    }

    if (showShareDialog && conversationId.isNotBlank()) {
        ShareConversationDialog(
            conversationId = conversationId,
            onDismiss = { showShareDialog = false },
        )
    }

    if (showSignOutDialog) {
        SignOutConfirmDialog(
            onConfirm = {
                showSignOutDialog = false
                onSignOut()
            },
            onDismiss = { showSignOutDialog = false },
        )
    }
}

/** One of the drawer's top-row buttons (New chat / Incognito): an equal-width,
 *  bordered, centered text button. `active` tints it navy (incognito on). */
@Composable
private fun DrawerActionButton(
    label: String,
    active: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(8.dp))
            .border(
                width = 1.dp,
                color = if (active) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.outlineVariant,
                shape = RoundedCornerShape(8.dp),
            )
            .clickable(onClick = onClick)
            .padding(vertical = 10.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium.copy(fontWeight = FontWeight.Medium),
            color = if (active) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
        )
    }
}

@Composable
private fun SignOutConfirmDialog(
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.chat_sign_out_confirm_title)) },
        text = { Text(stringResource(R.string.chat_sign_out_confirm_message)) },
        confirmButton = {
            TextButton(onClick = onConfirm) {
                Text(
                    stringResource(R.string.chat_sign_out),
                    color = MaterialTheme.colorScheme.error,
                )
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.common_cancel))
            }
        },
    )
}

@Composable
private fun ProviderMenu(
    state: ChatUiState,
    onSelect: (ProviderInfo) -> Unit,
    onRetry: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var expanded by remember { mutableStateOf(false) }
    Box(modifier = modifier) {
    TextButton(
        onClick = { expanded = true },
        shape = RoundedCornerShape(10.dp),
        contentPadding = PaddingValues(horizontal = 8.dp, vertical = 4.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        val selectModel = stringResource(R.string.chat_select_model)
        Text(
            text = state.selected?.label?.ifBlank { selectModel } ?: selectModel,
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Icon(
            Icons.Outlined.ArrowDropDown,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            if (state.providers.isEmpty()) {
                if (state.error != null) {
                    DropdownMenuItem(
                        text = {
                            Text(
                                stringResource(R.string.common_retry),
                                color = MaterialTheme.colorScheme.primary,
                            )
                        },
                        onClick = {
                            expanded = false
                            onRetry()
                        },
                    )
                } else {
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.chat_no_providers)) },
                        onClick = { expanded = false },
                    )
                }
            } else {
                state.providers.forEach { provider ->
                    DropdownMenuItem(
                        text = { Text(provider.label) },
                        leadingIcon = when {
                            provider.isManageEntry ->
                                { { Icon(Icons.Outlined.Build, contentDescription = null) } }
                            provider.modelSpec != null ->
                                { { Icon(Icons.Outlined.Lock, contentDescription = null) } }
                            else -> null
                        },
                        onClick = {
                            onSelect(provider)
                            expanded = false
                        },
                    )
                }
            }
    }
    }
}

/**
 * Composer affordance that rewrites the draft on-device via Gemini Nano (ML Kit).
 * Tapping opens a small tone menu; while rewriting it shows a spinner.
 */
@Composable
private fun PolishButton(
    enabled: Boolean,
    polishing: Boolean,
    onPolish: (MlKitTextService.Tone) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    IconButton(
        onClick = { expanded = true },
        enabled = enabled && !polishing,
    ) {
        if (polishing) {
            CircularProgressIndicator(
                modifier = Modifier.size(20.dp),
                strokeWidth = 2.dp,
            )
        } else {
            Icon(
                imageVector = Icons.Outlined.AutoFixHigh,
                contentDescription = stringResource(R.string.chat_polish),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
        val tones = listOf(
            MlKitTextService.Tone.Rephrase to R.string.chat_polish_rephrase,
            MlKitTextService.Tone.Shorten to R.string.chat_polish_shorten,
            MlKitTextService.Tone.Professional to R.string.chat_polish_professional,
            MlKitTextService.Tone.Friendly to R.string.chat_polish_friendly,
        )
        tones.forEach { (tone, label) ->
            DropdownMenuItem(
                text = { Text(stringResource(label)) },
                onClick = {
                    expanded = false
                    onPolish(tone)
                },
            )
        }
    }
}

/**
 * Manage the on-device models: explains the privacy trade-off, then lists the catalog
 * (Gemma, Qwen, …) with a per-model (resumable) download / progress / delete. A model
 * appears in the provider picker once it finishes downloading.
 */
@Composable
private fun OnDeviceModelDialog(
    statuses: Map<String, OnDeviceModelStatus>,
    imported: List<OnDeviceModelSpec>,
    hasStorageAccess: () -> Boolean,
    onDownload: (OnDeviceModelSpec) -> Unit,
    onDelete: (OnDeviceModelSpec) -> Unit,
    onImport: (Uri) -> Unit,
    onDeleteImported: (OnDeviceModelSpec) -> Unit,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    var storageOk by remember { mutableStateOf(hasStorageAccess()) }

    // SAF picker: any file type, since .litertlm/.task/.gguf have no registered MIME.
    val pickLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri -> if (uri != null) onImport(uri) }

    // All-Files-Access lives in Settings (API 30+); returning re-checks the grant.
    val settingsLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult(),
    ) { storageOk = hasStorageAccess() }
    val permLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { storageOk = hasStorageAccess() }

    fun requestStorage() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            runCatching {
                settingsLauncher.launch(
                    Intent(
                        Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + context.packageName),
                    ),
                )
            }.onFailure {
                settingsLauncher.launch(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            }
        } else {
            permLauncher.launch(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        icon = { Icon(Icons.Outlined.Lock, contentDescription = null) },
        title = { Text(stringResource(R.string.chat_ondevice_title)) },
        text = {
            Column {
                Text(stringResource(R.string.chat_ondevice_desc))
                Spacer(Modifier.height(12.dp))

                // Downloads and file imports both write/read shared storage (so models
                // survive an uninstall); prompt for the one-time grant when missing.
                if (!storageOk) {
                    Text(
                        stringResource(R.string.chat_ondevice_storage_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    TextButton(onClick = { requestStorage() }) {
                        Text(stringResource(R.string.chat_ondevice_grant_storage))
                    }
                    Spacer(Modifier.height(4.dp))
                }

                OnDeviceModel.CATALOG.forEach { spec ->
                    OnDeviceModelRow(
                        spec = spec,
                        status = statuses[spec.id] ?: OnDeviceModelStatus.Absent,
                        onDownload = { onDownload(spec) },
                        onDelete = { onDelete(spec) },
                    )
                    Spacer(Modifier.height(10.dp))
                }

                HorizontalDivider(Modifier.padding(vertical = 4.dp))
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(
                        stringResource(R.string.chat_ondevice_imported),
                        style = MaterialTheme.typography.titleSmall,
                        modifier = Modifier.weight(1f),
                    )
                    TextButton(
                        onClick = { pickLauncher.launch(arrayOf("*/*")) },
                        enabled = storageOk,
                    ) { Text(stringResource(R.string.chat_ondevice_import)) }
                }
                imported.forEach { spec ->
                    ImportedModelRow(spec = spec, onDelete = { onDeleteImported(spec) })
                    Spacer(Modifier.height(8.dp))
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_done)) }
        },
    )
}

/** One imported-model row: name + size, with a "forget" action (keeps the user's file). */
@Composable
private fun ImportedModelRow(
    spec: OnDeviceModelSpec,
    onDelete: () -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(modifier = Modifier.weight(1f)) {
            Text(spec.displayName, style = MaterialTheme.typography.titleSmall)
            Text(
                "~" + formatBytes(spec.approxBytes) + " · " + spec.fileName,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        TextButton(onClick = onDelete) {
            Text(
                stringResource(R.string.chat_ondevice_remove),
                color = MaterialTheme.colorScheme.error,
            )
        }
    }
}

/** One catalog row: model name + its download/progress/delete affordance. */
@Composable
private fun OnDeviceModelRow(
    spec: OnDeviceModelSpec,
    status: OnDeviceModelStatus,
    onDownload: () -> Unit,
    onDelete: () -> Unit,
) {
    Column {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(spec.displayName, style = MaterialTheme.typography.titleSmall)
                Text(
                    "~" + formatBytes(spec.approxBytes) + " · " + spec.recommendedRam + " RAM",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            when (status) {
                is OnDeviceModelStatus.Ready -> TextButton(onClick = onDelete) {
                    Text(
                        stringResource(R.string.chat_ondevice_delete),
                        color = MaterialTheme.colorScheme.error,
                    )
                }
                is OnDeviceModelStatus.Downloading -> Text(
                    if (status.totalBytes > 0) "${(status.fraction * 100).toInt()}%" else "…",
                    style = MaterialTheme.typography.bodySmall,
                )
                is OnDeviceModelStatus.Failed -> TextButton(onClick = onDownload) {
                    Text(stringResource(R.string.chat_ondevice_retry))
                }
                is OnDeviceModelStatus.Absent -> TextButton(onClick = onDownload) {
                    Text(stringResource(R.string.chat_ondevice_download))
                }
            }
        }
        when (status) {
            is OnDeviceModelStatus.Downloading -> {
                if (status.totalBytes > 0) {
                    LinearProgressIndicator(
                        progress = { status.fraction },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(Modifier.height(4.dp))
                    Text(
                        "${formatBytes(status.downloadedBytes)} / ${formatBytes(status.totalBytes)}",
                        style = MaterialTheme.typography.bodySmall,
                    )
                } else {
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }
            }
            is OnDeviceModelStatus.Failed -> Text(
                status.message,
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall,
            )
            is OnDeviceModelStatus.Ready -> Text(
                stringResource(R.string.chat_ondevice_ready),
                color = MaterialTheme.colorScheme.primary,
                style = MaterialTheme.typography.bodySmall,
            )
            is OnDeviceModelStatus.Absent -> Unit
        }
    }
}

/** Human-readable byte count (e.g. "2.5 GB"), locale-agnostic. */
private fun formatBytes(bytes: Long): String {
    if (bytes <= 0) return "0 B"
    val units = arrayOf("B", "KB", "MB", "GB", "TB")
    var value = bytes.toDouble()
    var i = 0
    while (value >= 1024 && i < units.lastIndex) {
        value /= 1024
        i++
    }
    return if (i == 0) "$bytes B" else String.format(java.util.Locale.US, "%.1f %s", value, units[i])
}

@Composable
private fun MessageBubble(msg: ChatMessage) {
    val isUser = msg.role == Role.User
    val bubbleMaxWidth = LocalLayoutInfo.current.bubbleMaxWidth
    var showStats by remember(msg.id) { mutableStateOf(false) }
    val clipboard = LocalClipboardManager.current
    Column(
        modifier = Modifier.fillMaxWidth(),
        horizontalAlignment = if (isUser) Alignment.End else Alignment.Start,
    ) {
        if (msg.thinking.isNotEmpty()) {
            Surface(
                shape = RoundedCornerShape(10.dp),
                color = MaterialTheme.colorScheme.surfaceContainerLow,
                modifier = Modifier
                    .widthIn(max = bubbleMaxWidth)
                    .padding(bottom = 6.dp),
            ) {
                Text(
                    text = msg.thinking,
                    style = MaterialTheme.typography.bodySmall.copy(fontStyle = FontStyle.Italic),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
                )
            }
        }
        if (isUser) {
            // Solid navy bubble with light text — the web client's user-turn style:
            // 8dp corners with a sharp 2dp "tail" at the top-end (top-right in LTR).
            val userShape = RoundedCornerShape(topStart = 8.dp, topEnd = 2.dp, bottomEnd = 8.dp, bottomStart = 8.dp)
            Surface(
                shape = userShape,
                color = BrandBlue,
                contentColor = Color.White,
                modifier = Modifier.widthIn(max = bubbleMaxWidth),
            ) {
                BubbleContent(msg)
            }
        } else {
            // AI: no bubble / no border — plain text on background, flush-left so it
            // (and the provider footer below) line up with the message column edge.
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 2.dp),
            ) {
                BubbleContent(msg, horizontalPadding = 0.dp)
                // Footer for a settled reply: the provider ("Agent/model") label on
                // the left; on the right the stats icon (when cost data is available)
                // and a copy icon. Mirrors the web client's reply footer.
                val showStatsIcon = msg.costStats != null
                val showCopyIcon = msg.text.isNotEmpty()
                val showLabel = msg.provider.isNotBlank()
                if (!msg.streaming && (showLabel || showStatsIcon || showCopyIcon)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            text = msg.provider,
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            modifier = Modifier.weight(1f),
                        )
                        if (showStatsIcon) {
                            IconButton(
                                onClick = { showStats = true },
                                modifier = Modifier.size(28.dp),
                            ) {
                                Icon(
                                    imageVector = Icons.Outlined.Info,
                                    contentDescription = stringResource(R.string.chat_stats_cd),
                                    tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.35f),
                                    modifier = Modifier.size(16.dp),
                                )
                            }
                        }
                        if (showCopyIcon) {
                            IconButton(
                                onClick = { clipboard.setText(AnnotatedString(msg.text)) },
                                modifier = Modifier.size(28.dp),
                            ) {
                                Icon(
                                    imageVector = Icons.Outlined.ContentCopy,
                                    contentDescription = stringResource(R.string.chat_copy_reply),
                                    tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.35f),
                                    modifier = Modifier.size(16.dp),
                                )
                            }
                        }
                    }
                }
            }
        }
    }
    if (showStats) {
        msg.costStats?.let {
            CostStatsDialog(
                stats = it,
                onDismiss = { showStats = false },
            )
        }
    }
}

@Composable
private fun CostStatsDialog(
    stats: CostStatistics,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.chat_stats_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                StatsRow(stringResource(R.string.chat_stats_model), stats.model)
                StatsRow(stringResource(R.string.chat_stats_input_tokens), stats.inputTokens.toString())
                StatsRow(stringResource(R.string.chat_stats_output_tokens), stats.outputTokens.toString())
                if (stats.thoughtTokens > 0) {
                    StatsRow(stringResource(R.string.chat_stats_thought_tokens), stats.thoughtTokens.toString())
                }
                StatsRow(stringResource(R.string.chat_stats_total_tokens), stats.totalTokens.toString())
                StatsRow(stringResource(R.string.chat_stats_total_cost), formatCost(stats.totalCost))
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.common_close))
            }
        },
    )
}

@Composable
private fun StatsRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}

/** Format USD cost with 4 fractional digits — covers typical LLM call ranges without noise. */
private fun formatCost(cost: Double): String = "$" + "%.4f".format(cost)

@Composable
private fun BubbleContent(msg: ChatMessage, horizontalPadding: Dp = 14.dp) {
    val context = LocalContext.current
    var viewerUrl by remember { mutableStateOf<String?>(null) }
    Column(modifier = Modifier.padding(horizontal = horizontalPadding, vertical = 10.dp)) {
        msg.attachmentNames.forEach { name ->
            // Uses LocalContentColor so it stays legible on the navy user bubble.
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(bottom = 4.dp),
            ) {
                Icon(
                    imageVector = Icons.Outlined.Description,
                    contentDescription = null,
                    tint = LocalContentColor.current.copy(alpha = 0.8f),
                    modifier = Modifier.size(14.dp),
                )
                Text(
                    text = name,
                    style = MaterialTheme.typography.bodySmall,
                    color = LocalContentColor.current.copy(alpha = 0.9f),
                    maxLines = 1,
                    modifier = Modifier.padding(start = 6.dp),
                )
            }
        }
        msg.toolCalls.forEachIndexed { index, tool ->
            ToolCallCard(
                tool = tool,
                modifier = if (index > 0) Modifier.padding(top = 6.dp) else Modifier,
            )
        }
        if (msg.text.isNotEmpty()) {
            MarkdownText(
                text = msg.text,
                style = MaterialTheme.typography.bodyMedium,
                modifier = if (msg.toolCalls.isNotEmpty()) Modifier.padding(top = 8.dp) else Modifier,
            )
        } else if (msg.streaming && msg.error == null && msg.toolCalls.isEmpty() &&
            msg.imageUrls.isEmpty() && msg.charts.isEmpty()
        ) {
            TypingDots()
        }
        msg.charts.forEachIndexed { index, option ->
            val needsTopPad = index > 0 || msg.text.isNotEmpty() || msg.toolCalls.isNotEmpty()
            EChartsView(
                optionJson = option,
                modifier = Modifier.padding(top = if (needsTopPad) 8.dp else 0.dp),
            )
        }
        msg.imageUrls.forEachIndexed { index, url ->
            val needsTopPad = index > 0 || msg.text.isNotEmpty() ||
                msg.toolCalls.isNotEmpty() || msg.charts.isNotEmpty()
            AsyncImage(
                model = ImageRequest.Builder(context)
                    .data(chatImageModel(url))
                    .crossfade(true)
                    .build(),
                contentDescription = null,
                contentScale = ContentScale.FillWidth,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = if (needsTopPad) 8.dp else 0.dp)
                    .clickable { viewerUrl = url },
            )
        }
        msg.error?.let {
            Text(
                text = it,
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
    }
    viewerUrl?.let { url ->
        ImageViewerDialog(url = url, onDismiss = { viewerUrl = null })
    }
}

@Composable
private fun ToolCallCard(tool: ToolCall, modifier: Modifier = Modifier) {
    var expanded by remember(tool.id) { mutableStateOf(false) }
    val hasDetails = tool.argumentsJson.isNotEmpty() || tool.resultJson.isNotEmpty()
    val toolFallback = stringResource(R.string.chat_tool)
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = MaterialTheme.colorScheme.surfaceContainerLow,
        modifier = modifier
            .fillMaxWidth()
            .clickable(enabled = hasDetails) { expanded = !expanded },
    ) {
        Column(modifier = Modifier.padding(horizontal = 10.dp, vertical = 8.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(
                    imageVector = Icons.Outlined.Build,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.size(16.dp),
                )
                Text(
                    text = tool.title.ifBlank { toolFallback },
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier
                        .weight(1f)
                        .padding(start = 8.dp),
                )
                if (!tool.resultReceived) {
                    CircularProgressIndicator(
                        strokeWidth = 1.5.dp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(12.dp),
                    )
                } else if (hasDetails) {
                    Icon(
                        imageVector = if (expanded) Icons.Outlined.ExpandLess else Icons.Outlined.ExpandMore,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(18.dp),
                    )
                }
            }
            if (expanded) {
                if (tool.argumentsJson.isNotEmpty()) {
                    ToolCallSection(label = stringResource(R.string.chat_tool_args), content = tool.argumentsJson)
                }
                if (tool.resultJson.isNotEmpty()) {
                    ToolCallSection(label = stringResource(R.string.chat_tool_result), content = tool.resultJson)
                }
            }
        }
    }
}

@Composable
private fun ToolCallSection(label: String, content: String) {
    Column(modifier = Modifier.padding(top = 8.dp)) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Surface(
            shape = RoundedCornerShape(6.dp),
            color = MaterialTheme.colorScheme.surface,
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 4.dp),
        ) {
            Text(
                text = content,
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                color = MaterialTheme.colorScheme.onSurface,
                modifier = Modifier.padding(horizontal = 8.dp, vertical = 6.dp),
            )
        }
    }
}

@Composable
private fun TypingDots() {
    val transition = rememberInfiniteTransition(label = "typing")
    Row(
        modifier = Modifier.padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for (i in 0..2) {
            val alpha by transition.animateFloat(
                initialValue = 0.25f,
                targetValue = 1f,
                animationSpec = infiniteRepeatable(
                    animation = tween(durationMillis = 700, delayMillis = i * 180),
                    repeatMode = RepeatMode.Reverse,
                ),
                label = "dot-$i",
            )
            Box(
                modifier = Modifier
                    .padding(horizontal = 2.dp)
                    .size(6.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = alpha)),
            )
        }
    }
}

// ---------------------------------------------------------------------------
// Compose previews.
//
// This app has no layout XML, so the IDE's Design pane is blank unless a
// @Preview exists. These cover the chat screen's self-contained pieces — the
// ones that take plain parameters and touch neither LocalAppContainer nor a
// ViewModel, which is what makes them renderable without a running app.
//
// ChatScreen itself is deliberately not previewed: it builds its ViewModel from
// LocalAppContainer, and AppContainer constructs OkHttp / Retrofit / DataStore /
// ModelManager against a real Context. Previewing a whole screen would mean
// extracting an interface for it first.
// ---------------------------------------------------------------------------

@Preview(name = "Top bar brand", showBackground = true)
@Composable
private fun WordmarkPreview() {
    MirobodyTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = {}) {
                    Icon(
                        Icons.Outlined.Menu,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Wordmark()
            }
        }
    }
}

@Preview(name = "Subject picker", showBackground = true)
@Composable
private fun SubjectPickerPreview() {
    MirobodyTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            SubjectPicker(
                sharers = listOf(
                    HealthSharer(member = 1, email = "mom@example.com", nickname = "Mom"),
                    HealthSharer(member = 2, email = "dad@example.com", nickname = "Dad"),
                ),
                subject = 1,
                onSelect = {},
            )
        }
    }
}

@Preview(name = "Empty state · light", showBackground = true, heightDp = 320)
@Preview(
    name = "Empty state · dark",
    showBackground = true,
    heightDp = 320,
    uiMode = Configuration.UI_MODE_NIGHT_YES,
)
@Composable
private fun EmptyStatePreview() {
    MirobodyTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            EmptyState(incognito = false)
        }
    }
}

@Preview(name = "Empty state · incognito", showBackground = true, heightDp = 320)
@Composable
private fun EmptyStateIncognitoPreview() {
    MirobodyTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            EmptyState(incognito = true)
        }
    }
}

@Preview(name = "Incognito banner", showBackground = true)
@Composable
private fun IncognitoBannerPreview() {
    MirobodyTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            IncognitoBanner()
        }
    }
}

@Preview(name = "Drawer top row", showBackground = true)
@Composable
private fun DrawerActionButtonsPreview() {
    MirobodyTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                DrawerActionButton(
                    label = "New chat",
                    active = false,
                    onClick = {},
                    modifier = Modifier.weight(1f),
                )
                // Incognito on: navy border + navy label, so "am I being recorded"
                // is answerable at a glance.
                DrawerActionButton(
                    label = "Incognito",
                    active = true,
                    onClick = {},
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}
