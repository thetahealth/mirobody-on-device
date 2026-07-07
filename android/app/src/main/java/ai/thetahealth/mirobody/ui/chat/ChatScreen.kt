package ai.thetahealth.mirobody.ui.chat

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
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
import androidx.compose.foundation.layout.widthIn
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
import androidx.compose.material.icons.outlined.Settings
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import ai.thetahealth.mirobody.R
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.data.chat.dto.ChatAttachment
import ai.thetahealth.mirobody.data.chat.dto.CostStatistics
import ai.thetahealth.mirobody.data.chat.dto.ProviderInfo
import ai.thetahealth.mirobody.data.llm.MlKitTextService
import ai.thetahealth.mirobody.data.llm.OnDeviceModelStatus
import ai.thetahealth.mirobody.data.circle.dto.HealthSharer
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.LocalLayoutInfo
import ai.thetahealth.mirobody.ui.LocalFontSizePreview
import ai.thetahealth.mirobody.ui.ProvideLocale
import ai.thetahealth.mirobody.ui.circle.CareCircleDialog
import ai.thetahealth.mirobody.ui.circle.ShareConversationDialog
import ai.thetahealth.mirobody.ui.health.BleDeviceDialog
import ai.thetahealth.mirobody.ui.health.HealthSyncDialog
import ai.thetahealth.mirobody.ui.settings.BaseUrlDialog
import ai.thetahealth.mirobody.ui.settings.FontSizeDialog
import ai.thetahealth.mirobody.ui.settings.LanguageDialog
import ai.thetahealth.mirobody.ui.theme.BrandBlue
import coil.compose.AsyncImage
import coil.request.ImageRequest
import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(
    onSignOut: () -> Unit,
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
    val baseUrl by container.settings.baseUrl.collectAsState(initial = null)
    val listState = rememberLazyListState()
    val scope = rememberCoroutineScope()
    val drawerState = rememberDrawerState(initialValue = DrawerValue.Closed)
    val context = LocalContext.current

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

    LaunchedEffect(state.messages.size, state.messages.lastOrNull()?.text?.length) {
        if (state.messages.isNotEmpty()) {
            listState.animateScrollToItem(state.messages.lastIndex)
        }
    }

    // On-device model management dialog. Opens when the user picks the on-device
    // provider (or tries to send) before the model has been downloaded.
    var showModelDialog by remember { mutableStateOf(false) }
    val onProviderSelected: (ProviderInfo) -> Unit = { provider ->
        vm.onProviderSelected(provider)
        if (provider.isOnDevice && state.onDeviceModel !is OnDeviceModelStatus.Ready) {
            showModelDialog = true
        }
    }
    if (showModelDialog) {
        OnDeviceModelDialog(
            status = state.onDeviceModel,
            onDownload = vm::downloadOnDeviceModel,
            onDelete = vm::deleteOnDeviceModel,
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
                HistoryScreen(
                    onBack = { scope.launch { drawerState.close() } },
                    isActive = drawerState.targetValue == DrawerValue.Open,
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
                navigationIcon = {
                    BrandLogo(baseUrl = baseUrl, onClick = { scope.launch { drawerState.open() } })
                },
                title = { ProviderMenu(state, onProviderSelected, vm::loadProviders) },
                actions = {
                    val fontOffset by container.settings.fontSizeOffset.collectAsState(initial = 0)
                    SettingsMenu(
                        currentLanguage = state.language,
                        currentFontOffset = fontOffset,
                        conversationId = state.conversationId,
                        onSelectLanguage = { code ->
                            scope.launch { container.settings.setLanguage(code) }
                        },
                        onSelectFontOffset = { offset ->
                            scope.launch { container.settings.setFontSizeOffset(offset) }
                        },
                        onSignOut = {
                            scope.launch {
                                container.authRepository.signOut()
                                container.chatHistoryStore.clear()   // don't leave one user's chat for the next
                                onSignOut()
                            }
                        },
                    )
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
                    Column(modifier = Modifier.widthIn(max = layout.contentMaxWidth)) {
                        // "Currently for" subject picker — shown only when care-circle
                        // members have shared their health data with this user. Picking
                        // one sends `subject` so the AI's family_health tool defaults to
                        // that member ("how is Mom doing?").
                        if (state.sharers.isNotEmpty()) {
                            SubjectPicker(
                                sharers = state.sharers,
                                subject = state.subject,
                                onSelect = vm::onSubjectSelected,
                            )
                        }
                        // Staged attachments, removable until the turn is sent.
                        if (state.attachments.isNotEmpty()) {
                            AttachmentChips(
                                attachments = state.attachments,
                                onRemove = vm::removeAttachment,
                            )
                        }
                        // Single-line composer, so center the paperclip against the field.
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            IconButton(
                                onClick = { filePicker.launch("*/*") },
                                enabled = !state.sending,
                                modifier = Modifier.padding(start = 4.dp),
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
                            ChatInputField(
                                value = state.input,
                                onValueChange = vm::onInputChange,
                                enabled = !state.sending,
                                canSend = !state.sending && state.selected != null &&
                                    (state.input.isNotBlank() || state.attachments.isNotEmpty()),
                                onSend = {
                                    // For the on-device provider, prompt to download the
                                    // model first instead of sending into a dead engine.
                                    if (state.selected?.isOnDevice == true &&
                                        state.onDeviceModel !is OnDeviceModelStatus.Ready
                                    ) {
                                        showModelDialog = true
                                    } else {
                                        vm.send()
                                    }
                                },
                                modifier = Modifier.weight(1f),
                            )
                        }
                    }
                }
            }
        },
    ) { padding ->
        Column(modifier = Modifier.fillMaxSize().padding(padding)) {
            if (state.messages.isEmpty()) {
                EmptyState(modifier = Modifier.weight(1f))
            } else {
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    contentAlignment = Alignment.TopCenter,
                ) {
                    LazyColumn(
                        state = listState,
                        modifier = Modifier
                            .fillMaxSize()
                            .widthIn(max = layout.contentMaxWidth)
                            .padding(horizontal = layout.screenPadding),
                        contentPadding = PaddingValues(vertical = layout.screenPadding),
                        verticalArrangement = Arrangement.spacedBy(layout.messageSpacing),
                    ) {
                        items(state.messages, key = { it.id }) { msg ->
                            MessageBubble(msg, currentLanguage = state.language)
                        }
                    }
                }
            }
        }
    }
    }
}

@Composable
private fun BrandLogo(baseUrl: String?, onClick: () -> Unit) {
    val url = baseUrl?.trimEnd('/')?.let { "$it/mirobody.svg" }
    val context = LocalContext.current
    IconButton(
        onClick = onClick,
        modifier = Modifier.padding(start = 4.dp),
    ) {
        Box(
            modifier = Modifier
                .size(32.dp)
                .clip(CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            if (url != null) {
                AsyncImage(
                    model = ImageRequest.Builder(context)
                        .data(url)
                        .crossfade(true)
                        .build(),
                    contentDescription = stringResource(R.string.chat_history_cd),
                    modifier = Modifier.fillMaxSize(),
                )
            }
        }
    }
}

@Composable
private fun EmptyState(modifier: Modifier = Modifier) {
    Box(modifier = modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(6.dp),
            modifier = Modifier.padding(24.dp),
        ) {
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

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ChatInputField(
    value: String,
    onValueChange: (String) -> Unit,
    enabled: Boolean,
    canSend: Boolean,
    onSend: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val interactionSource = remember { MutableInteractionSource() }
    val shape = RoundedCornerShape(16.dp)
    val accent = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f)
    val colors = OutlinedTextFieldDefaults.colors(
        focusedBorderColor = accent,
        unfocusedBorderColor = accent.copy(alpha = 0.35f),
        focusedContainerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        unfocusedContainerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        disabledContainerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        cursorColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f),
    )
    val hint = stringResource(R.string.chat_message_hint)
    val hintColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
    BasicTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 10.dp),
        enabled = enabled,
        singleLine = true,
        textStyle = MaterialTheme.typography.bodyLarge.copy(color = MaterialTheme.colorScheme.onSurface),
        cursorBrush = SolidColor(MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)),
        keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
        keyboardActions = KeyboardActions(onSend = { if (canSend) onSend() }),
        interactionSource = interactionSource,
        decorationBox = { innerTextField ->
            OutlinedTextFieldDefaults.DecorationBox(
                value = value,
                innerTextField = innerTextField,
                enabled = enabled,
                singleLine = true,
                visualTransformation = VisualTransformation.None,
                interactionSource = interactionSource,
                placeholder = @Composable { Text(hint, color = hintColor) },
                colors = colors,
                container = {
                    OutlinedTextFieldDefaults.Container(
                        enabled = enabled,
                        isError = false,
                        interactionSource = interactionSource,
                        colors = colors,
                        shape = shape,
                        focusedBorderThickness = 1.dp,
                        unfocusedBorderThickness = 0.5.dp,
                    )
                },
            )
        },
    )
}

/** Label for a sharer in the subject picker: nickname, else email, else "#handle". */
private fun HealthSharer.displayLabel(): String =
    nickname.ifBlank { email }.ifBlank { "#$member" }

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
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = stringResource(R.string.chat_currently_for),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Box {
            TextButton(
                onClick = { expanded = true },
                shape = RoundedCornerShape(10.dp),
            ) {
                Text(
                    text = selectedLabel,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface,
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

@Composable
private fun SettingsMenu(
    currentLanguage: String,
    currentFontOffset: Int,
    conversationId: String,
    onSelectLanguage: (String) -> Unit,
    onSelectFontOffset: (Int) -> Unit,
    onSignOut: () -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    var showLanguageDialog by remember { mutableStateOf(false) }
    var showFontSizeDialog by remember { mutableStateOf(false) }
    var showBackendDialog by remember { mutableStateOf(false) }
    var showHealthDialog by remember { mutableStateOf(false) }
    var showBleDialog by remember { mutableStateOf(false) }
    var showCircleDialog by remember { mutableStateOf(false) }
    var showShareDialog by remember { mutableStateOf(false) }
    var showAboutDialog by remember { mutableStateOf(false) }
    var showSignOutDialog by remember { mutableStateOf(false) }
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
            DropdownMenuItem(
                text = { Text(stringResource(R.string.chat_sync_health)) },
                onClick = {
                    expanded = false
                    showHealthDialog = true
                },
            )
            DropdownMenuItem(
                text = { Text(stringResource(R.string.chat_bluetooth)) },
                onClick = {
                    expanded = false
                    showBleDialog = true
                },
            )
            DropdownMenuItem(
                text = { Text(stringResource(R.string.chat_care_circle)) },
                onClick = {
                    expanded = false
                    showCircleDialog = true
                },
            )
            if (conversationId.isNotBlank()) {
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.chat_share)) },
                    onClick = {
                        expanded = false
                        showShareDialog = true
                    },
                )
            }
            DropdownMenuItem(
                text = { Text(stringResource(R.string.chat_about)) },
                onClick = {
                    expanded = false
                    showAboutDialog = true
                },
            )
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f))
            DropdownMenuItem(
                text = {
                    Text(
                        stringResource(R.string.chat_sign_out),
                        color = MaterialTheme.colorScheme.error,
                    )
                },
                onClick = {
                    expanded = false
                    showSignOutDialog = true
                },
            )
        }
    }

    if (showLanguageDialog) {
        LanguageDialog(
            current = currentLanguage,
            onPick = { code ->
                onSelectLanguage(code)
                showLanguageDialog = false
            },
            onDismiss = { showLanguageDialog = false },
        )
    }

    val fontSizePreview = LocalFontSizePreview.current
    if (showFontSizeDialog) {
        FontSizeDialog(
            currentLanguage = currentLanguage,
            current = currentFontOffset,
            onPreview = { offset -> fontSizePreview.value = offset },
            onPick = { offset ->
                // Keep the preview set to the picked value so the UI doesn't snap
                // back to the persisted size while DataStore is still writing.
                // MainActivity clears it once persisted catches up.
                onSelectFontOffset(offset)
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

    if (showHealthDialog) {
        HealthSyncDialog(
            currentLanguage = currentLanguage,
            onDismiss = { showHealthDialog = false },
        )
    }

    if (showBleDialog) {
        BleDeviceDialog(
            currentLanguage = currentLanguage,
            onDismiss = { showBleDialog = false },
        )
    }

    if (showCircleDialog) {
        CareCircleDialog(
            currentLanguage = currentLanguage,
            onDismiss = { showCircleDialog = false },
        )
    }

    if (showShareDialog && conversationId.isNotBlank()) {
        ShareConversationDialog(
            conversationId = conversationId,
            currentLanguage = currentLanguage,
            onDismiss = { showShareDialog = false },
        )
    }

    if (showAboutDialog) {
        AboutDialog(
            currentLanguage = currentLanguage,
            onDismiss = { showAboutDialog = false },
        )
    }

    if (showSignOutDialog) {
        SignOutConfirmDialog(
            currentLanguage = currentLanguage,
            onConfirm = {
                showSignOutDialog = false
                onSignOut()
            },
            onDismiss = { showSignOutDialog = false },
        )
    }
}

@Composable
private fun AboutDialog(
    currentLanguage: String,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    val versionName = remember(context) {
        runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrNull().orEmpty()
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            ProvideLocale(currentLanguage) {
                Text(stringResource(R.string.chat_about))
            }
        },
        text = {
            ProvideLocale(currentLanguage) {
                Column {
                    Text(
                        text = stringResource(R.string.app_name),
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(
                        text = stringResource(R.string.about_version, versionName),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
        confirmButton = {
            ProvideLocale(currentLanguage) {
                TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.common_close))
                }
            }
        },
    )
}

@Composable
private fun SignOutConfirmDialog(
    currentLanguage: String,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            ProvideLocale(currentLanguage) {
                Text(stringResource(R.string.chat_sign_out_confirm_title))
            }
        },
        text = {
            ProvideLocale(currentLanguage) {
                Text(stringResource(R.string.chat_sign_out_confirm_message))
            }
        },
        confirmButton = {
            ProvideLocale(currentLanguage) {
                TextButton(onClick = onConfirm) {
                    Text(
                        stringResource(R.string.chat_sign_out),
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        dismissButton = {
            ProvideLocale(currentLanguage) {
                TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.common_cancel))
                }
            }
        },
    )
}

@Composable
private fun ProviderMenu(
    state: ChatUiState,
    onSelect: (ProviderInfo) -> Unit,
    onRetry: () -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    TextButton(
        onClick = { expanded = true },
        shape = RoundedCornerShape(10.dp),
    ) {
        val selectModel = stringResource(R.string.chat_select_model)
        Text(
            text = state.selected?.name?.ifBlank { selectModel } ?: selectModel,
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.onSurface,
        )
        Icon(
            Icons.Outlined.ArrowDropDown,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
        ProvideLocale(state.language) {
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
                        text = {
                            if (provider.isOnDevice) {
                                Column {
                                    Text(provider.name)
                                    Text(
                                        text = onDeviceStatusLabel(state.onDeviceModel),
                                        style = MaterialTheme.typography.bodySmall,
                                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    )
                                }
                            } else {
                                Text(provider.name)
                            }
                        },
                        leadingIcon = if (provider.isOnDevice) {
                            { Icon(Icons.Outlined.Lock, contentDescription = null) }
                        } else null,
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

/** One-line status shown under the on-device provider in the picker. */
@Composable
private fun onDeviceStatusLabel(status: OnDeviceModelStatus): String = when (status) {
    is OnDeviceModelStatus.Ready -> stringResource(R.string.chat_ondevice_ready)
    is OnDeviceModelStatus.Downloading ->
        "${stringResource(R.string.chat_ondevice_downloading)} ${(status.fraction * 100).toInt()}%"
    is OnDeviceModelStatus.Failed -> stringResource(R.string.chat_ondevice_failed)
    is OnDeviceModelStatus.Absent -> stringResource(R.string.chat_ondevice_absent)
}

/**
 * Manage the on-device Gemma 4 model: explains the privacy trade-off, drives the
 * (resumable) download with progress, and offers delete to reclaim storage.
 */
@Composable
private fun OnDeviceModelDialog(
    status: OnDeviceModelStatus,
    onDownload: () -> Unit,
    onDelete: () -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        icon = { Icon(Icons.Outlined.Lock, contentDescription = null) },
        title = { Text(stringResource(R.string.chat_ondevice_title)) },
        text = {
            Column {
                Text(stringResource(R.string.chat_ondevice_desc))
                Spacer(Modifier.height(12.dp))
                when (status) {
                    is OnDeviceModelStatus.Downloading -> {
                        if (status.totalBytes > 0) {
                            LinearProgressIndicator(
                                progress = { status.fraction },
                                modifier = Modifier.fillMaxWidth(),
                            )
                            Spacer(Modifier.height(6.dp))
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
        },
        confirmButton = {
            when (status) {
                is OnDeviceModelStatus.Ready -> TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.common_done))
                }
                is OnDeviceModelStatus.Downloading -> TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.chat_ondevice_continue_background))
                }
                is OnDeviceModelStatus.Failed -> TextButton(onClick = onDownload) {
                    Text(stringResource(R.string.chat_ondevice_retry))
                }
                is OnDeviceModelStatus.Absent -> TextButton(onClick = onDownload) {
                    Text(stringResource(R.string.chat_ondevice_download))
                }
            }
        },
        dismissButton = {
            if (status is OnDeviceModelStatus.Ready) {
                TextButton(onClick = { onDelete(); onDismiss() }) {
                    Text(
                        stringResource(R.string.chat_ondevice_delete),
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            } else {
                TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_cancel)) }
            }
        },
    )
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
private fun MessageBubble(msg: ChatMessage, currentLanguage: String) {
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
            // AI: no bubble / no border — plain text on background.
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 2.dp),
            ) {
                BubbleContent(msg)
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
                currentLanguage = currentLanguage,
                onDismiss = { showStats = false },
            )
        }
    }
}

@Composable
private fun CostStatsDialog(
    stats: CostStatistics,
    currentLanguage: String,
    onDismiss: () -> Unit,
) {
    // AlertDialog hosts its content in a new Window whose LocalContext is the bare
    // Activity context — outer ProvideLocale doesn't propagate, so we re-wrap each
    // slot. Same pattern as FontSizeDialog / AboutDialog above.
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            ProvideLocale(currentLanguage) {
                Text(stringResource(R.string.chat_stats_title))
            }
        },
        text = {
            ProvideLocale(currentLanguage) {
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
            }
        },
        confirmButton = {
            ProvideLocale(currentLanguage) {
                TextButton(onClick = onDismiss) {
                    Text(stringResource(R.string.common_close))
                }
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
private fun BubbleContent(msg: ChatMessage) {
    val context = LocalContext.current
    var viewerUrl by remember { mutableStateOf<String?>(null) }
    Column(modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp)) {
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
