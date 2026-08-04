package ai.thetahealth.mirobody.ui.chat

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.DeleteOutline
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.data.chat.dto.SessionSummary
import ai.thetahealth.mirobody.ui.LocalAppContainer
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/**
 * The history state plus the actions its rows need, hoisted so the chat drawer can
 * emit the rows into its OWN lazy list (see [historySection]) instead of hosting a
 * second scroller. The drawer's menu groups are ~585dp tall on their own, so a
 * history band that scrolled independently left everything below it unreachable on a
 * short screen, in landscape, or at a larger font size.
 */
internal class HistoryController(
    val state: HistoryUiState,
    val refresh: () -> Unit,
    val delete: (String) -> Unit,
)

/** Loads the history for the drawer, refreshed on each open. */
@Composable
internal fun rememberHistoryController(isActive: Boolean): HistoryController {
    val container = LocalAppContainer.current
    val vm: HistoryViewModel = viewModel(
        factory = viewModelFactory {
            initializer { HistoryViewModel(container.chatRepository, container.errorBus) }
        },
    )
    val state by vm.state.collectAsState()

    // Defer /api/history until the drawer actually opens; refresh on each open so
    // newly-created sessions show up without a manual retry.
    LaunchedEffect(isActive) {
        if (isActive) vm.refresh()
    }

    return remember(state) { HistoryController(state, vm::refresh, vm::deleteHistory) }
}

/**
 * The conversation rows — or the loading / error / empty placeholder standing in for
 * them — as items of the drawer's lazy list.
 */
internal fun LazyListScope.historySection(
    controller: HistoryController,
    onOpen: (String) -> Unit,
) {
    val state = controller.state
    when {
        state.loading && state.items.isEmpty() -> item("history-loading") {
            HistoryPlaceholder { CircularProgressIndicator() }
        }

        state.error != null -> item("history-error") {
            HistoryPlaceholder {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(
                        text = state.error.orEmpty(),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.error,
                    )
                    TextButton(onClick = controller.refresh) {
                        Text(stringResource(R.string.common_retry), color = MaterialTheme.colorScheme.primary)
                    }
                }
            }
        }

        state.items.isEmpty() -> item("history-empty") {
            HistoryPlaceholder {
                Text(
                    text = stringResource(R.string.history_empty),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        else -> items(state.items, key = { it.sessionId }) { item ->
            // A rule above each row (the last row has none below it).
            HorizontalDivider(
                color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.4f),
                modifier = Modifier.padding(horizontal = 20.dp),
            )
            HistoryRow(
                item,
                onOpen = { onOpen(item.sessionId) },
                onDelete = { controller.delete(item.sessionId) },
            )
        }
    }
}

/** Centred stand-in for the row list, tall enough to read as a band of its own. */
@Composable
private fun HistoryPlaceholder(content: @Composable () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 96.dp)
            .padding(24.dp),
        contentAlignment = Alignment.Center,
    ) {
        content()
    }
}

@Composable
private fun HistoryRow(item: SessionSummary, onOpen: () -> Unit, onDelete: () -> Unit) {
    val untitled = stringResource(R.string.history_untitled)
    val title = item.summary.takeIf { it.isNotBlank() }
        ?: item.sessionId.takeIf { it.isNotBlank() }
        ?: untitled
    var showConfirm by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 20.dp, end = 8.dp, top = 14.dp, bottom = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(
            modifier = Modifier
                .weight(1f)
                .clickable(onClick = onOpen),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Text(
                text = title,
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 2,
            )
            if (item.timestamp > 0L) {
                Text(
                    text = formatTimestamp(item.timestamp),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f),
                )
            }
        }
        IconButton(
            onClick = { showConfirm = true },
            modifier = Modifier.size(36.dp),
        ) {
            Icon(
                imageVector = Icons.Outlined.DeleteOutline,
                contentDescription = stringResource(R.string.history_delete_cd),
                tint = MaterialTheme.colorScheme.error,
                modifier = Modifier.size(18.dp),
            )
        }
    }
    if (showConfirm) {
        AlertDialog(
            onDismissRequest = { showConfirm = false },
            title = { Text(stringResource(R.string.history_delete_confirm_title)) },
            text = { Text(stringResource(R.string.history_delete_confirm_message)) },
            confirmButton = {
                TextButton(onClick = {
                    showConfirm = false
                    onDelete()
                }) {
                    Text(
                        stringResource(R.string.common_delete),
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            },
            dismissButton = {
                TextButton(onClick = { showConfirm = false }) {
                    Text(stringResource(R.string.common_cancel))
                }
            },
        )
    }
}

private val HISTORY_FORMATTER: DateTimeFormatter =
    DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm")

private fun formatTimestamp(millis: Long): String {
    // Backend returns the timestamp as epoch milliseconds (UTC).
    // Convert to the device's local zone for display.
    return try {
        Instant.ofEpochMilli(millis)
            .atZone(ZoneId.systemDefault())
            .format(HISTORY_FORMATTER)
    } catch (_: Exception) {
        millis.toString()
    }
}
