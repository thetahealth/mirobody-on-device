package ai.thetahealth.mirobody.ui.chat

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
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
import androidx.compose.material.icons.outlined.Build
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.ExpandLess
import androidx.compose.material.icons.outlined.ExpandMore
import androidx.compose.material.icons.outlined.Info
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
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
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
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalClipboardManager
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
import ai.thetahealth.mirobody.data.chat.dto.CostStatistics
import ai.thetahealth.mirobody.data.chat.dto.ProviderInfo
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.LocalLayoutInfo
import ai.thetahealth.mirobody.ui.LocalFontSizePreview
import ai.thetahealth.mirobody.ui.ProvideLocale
import ai.thetahealth.mirobody.ui.health.HealthSyncDialog
import ai.thetahealth.mirobody.ui.settings.BaseUrlDialog
import ai.thetahealth.mirobody.ui.settings.LanguageDialog
import ai.thetahealth.mirobody.ui.theme.BrandBlue
import coil.compose.AsyncImage
import coil.request.ImageRequest
import kotlin.math.roundToInt
import kotlinx.coroutines.launch

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
                    container.settings,
                    container.errorBus,
                    container.chatHistoryStore,
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

    LaunchedEffect(state.messages.size, state.messages.lastOrNull()?.text?.length) {
        if (state.messages.isNotEmpty()) {
            listState.animateScrollToItem(state.messages.lastIndex)
        }
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
                title = { ProviderMenu(state, vm::onProviderSelected, vm::loadProviders) },
                actions = {
                    val fontOffset by container.settings.fontSizeOffset.collectAsState(initial = 0)
                    SettingsMenu(
                        currentLanguage = state.language,
                        currentFontOffset = fontOffset,
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
                        ChatInputField(
                            value = state.input,
                            onValueChange = vm::onInputChange,
                            enabled = !state.sending,
                            canSend = !state.sending && state.input.isNotBlank() && state.selected != null,
                            onSend = vm::send,
                        )
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
        modifier = Modifier
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

@Composable
private fun SettingsMenu(
    currentLanguage: String,
    currentFontOffset: Int,
    onSelectLanguage: (String) -> Unit,
    onSelectFontOffset: (Int) -> Unit,
    onSignOut: () -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    var showLanguageDialog by remember { mutableStateOf(false) }
    var showFontSizeDialog by remember { mutableStateOf(false) }
    var showBackendDialog by remember { mutableStateOf(false) }
    var showHealthDialog by remember { mutableStateOf(false) }
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
private fun FontSizeDialog(
    currentLanguage: String,
    current: Int,
    onPreview: (Int) -> Unit,
    onPick: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val tiers = listOf(
        -4 to R.string.chat_font_size_smaller,
        -2 to R.string.chat_font_size_small,
        0 to R.string.chat_font_size_normal,
        2 to R.string.chat_font_size_large,
        4 to R.string.chat_font_size_larger,
    )
    val initialIndex = tiers.indexOfFirst { it.first == current }.let {
        if (it < 0) 2 else it
    }
    var stagedIndex by remember(current) { mutableStateOf(initialIndex) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            ProvideLocale(currentLanguage) {
                Text(stringResource(R.string.chat_font_size))
            }
        },
        text = {
            ProvideLocale(currentLanguage) {
                Column(modifier = Modifier.fillMaxWidth()) {
                    Slider(
                        value = stagedIndex.toFloat(),
                        onValueChange = { v ->
                            val idx = v.roundToInt().coerceIn(0, tiers.lastIndex)
                            if (idx != stagedIndex) {
                                stagedIndex = idx
                                onPreview(tiers[idx].first)
                            }
                        },
                        valueRange = 0f..tiers.lastIndex.toFloat(),
                        steps = tiers.size - 2,
                    )
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        tiers.forEachIndexed { index, (_, labelRes) ->
                            val selected = index == stagedIndex
                            Text(
                                text = stringResource(labelRes),
                                style = MaterialTheme.typography.labelSmall,
                                color = if (selected) MaterialTheme.colorScheme.primary
                                        else MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            ProvideLocale(currentLanguage) {
                TextButton(onClick = { onPick(tiers[stagedIndex].first) }) {
                    Text(stringResource(R.string.common_done))
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
                        text = { Text(provider.name) },
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
            val userShape = RoundedCornerShape(topStart = 14.dp, topEnd = 14.dp, bottomStart = 14.dp, bottomEnd = 4.dp)
            Surface(
                shape = userShape,
                color = BrandBlue.copy(alpha = 0.12f),
                contentColor = MaterialTheme.colorScheme.onSurface,
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
                // Footer for a settled reply: the stats icon (when cost data is
                // available) and, to its right, a copy icon.
                val showStatsIcon = msg.costStats != null
                val showCopyIcon = msg.text.isNotEmpty()
                if (!msg.streaming && (showStatsIcon || showCopyIcon)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.End,
                    ) {
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
