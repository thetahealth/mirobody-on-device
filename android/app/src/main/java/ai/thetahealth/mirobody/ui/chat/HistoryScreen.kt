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
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.DialogTitleWithClose
import ai.thetahealth.mirobody.data.chat.dto.SessionSummary
import ai.thetahealth.mirobody.ui.LocalAppContainer
import java.time.Instant
import java.time.ZoneId
import java.time.ZonedDateTime
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
            // "2 hr ago · 12 messages" — the same subtitle HarmonyOS's drawer carries.
            // Either half can be missing (a null timestamp, or a backend that does not
            // send message_count yet), so the separator is only drawn when both are
            // there rather than leaving a dangling "·".
            val stamp = if (item.timestamp > 0L) relativeStamp(item.timestamp) else ""
            val count = if (item.messageCount > 0) {
                pluralStringResource(
                    R.plurals.history_message_count,
                    item.messageCount,
                    item.messageCount,
                )
            } else ""
            val subtitle = listOf(stamp, count).filter { it.isNotEmpty() }.joinToString(" · ")
            if (subtitle.isNotEmpty()) {
                Text(
                    text = subtitle,
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
            title = { DialogTitleWithClose(stringResource(R.string.history_delete_confirm_title), { showConfirm = false }) },
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

/**
 * Relative for the recent past, absolute beyond a week — HarmonyOS's ladder, ported.
 *
 * A bare clock time on a three-week-old session reads as "today" at a glance, which is
 * worse than no timestamp; a bare date on something from ten minutes ago is equally
 * unhelpful. `yyyy-MM-dd HH:mm` (what this was) is the first of those for every row.
 *
 * Hand-rolled against app string resources rather than `DateUtils
 * .getRelativeTimeSpanString`, which reads the framework's own resources and therefore
 * follows the SYSTEM language. This app carries its own language setting applied through
 * `createConfigurationContext` and never calls `Locale.setDefault`, so the platform
 * helper would print "2 hours ago" in the phone's language beside UI in the user's.
 * Same reason the absolute fallback passes the configuration locale to the formatter
 * explicitly: a pattern from `values-en` must not be filled with month names from a
 * Chinese default locale.
 *
 * Not reactive, and does not need to be: the drawer reloads history every time it opens
 * (see rememberHistoryController), which is the only moment these are on screen.
 */
@Composable
private fun relativeStamp(millis: Long): String {
    val locale = LocalConfiguration.current.locales[0]
    val zoned = remember(millis) {
        runCatching { Instant.ofEpochMilli(millis).atZone(ZoneId.systemDefault()) }.getOrNull()
    } ?: return millis.toString()

    // Clamped at zero: a server clock a few seconds ahead of the phone would otherwise
    // render "-1 min ago".
    val mins = ((System.currentTimeMillis() - millis) / 60_000L).coerceAtLeast(0L)
    if (mins < 1) return stringResource(R.string.history_just_now)
    if (mins < 60) return pluralStringResource(
        R.plurals.history_mins_ago, mins.toInt(), mins.toInt(),
    )
    val hours = mins / 60
    if (hours < 24) return pluralStringResource(
        R.plurals.history_hours_ago, hours.toInt(), hours.toInt(),
    )
    val days = hours / 24
    if (days < 7) return pluralStringResource(
        R.plurals.history_days_ago, days.toInt(), days.toInt(),
    )

    // Past a week, an absolute date. The year is carried only when it is not the
    // current one — Harmony always omits it, which leaves "3/15" ambiguous once a
    // conversation is more than a year old.
    val sameYear = zoned.year == ZonedDateTime.now(ZoneId.systemDefault()).year
    val pattern = stringResource(
        if (sameYear) R.string.history_date_pattern else R.string.history_date_year_pattern
    )
    return remember(millis, pattern, locale) {
        runCatching { zoned.format(DateTimeFormatter.ofPattern(pattern, locale)) }
            .getOrDefault(millis.toString())
    }
}
