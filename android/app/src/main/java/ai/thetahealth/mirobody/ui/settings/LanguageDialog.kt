package ai.thetahealth.mirobody.ui.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.snapping.rememberSnapFlingBehavior
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.DialogTitleWithClose
import ai.thetahealth.mirobody.ui.theme.MirobodyTheme
import kotlin.math.abs
import kotlinx.coroutines.launch

val LANGUAGE_OPTIONS: List<Pair<String, String>> = listOf(
    "zh" to "中文",
    "ja" to "日本語",
    "ko" to "한국어",
    "en" to "English",
    "fr" to "Français",
    "de" to "Deutsch",
    "ru" to "Русский",
    "es" to "Español",
    "ar" to "العربية",
    // Hebrew: the resource folder and Locale code are the legacy "iw" (Java
    // normalizes "he" -> "iw"), so use "iw" here to match values-iw.
    "iw" to "עברית",
)

@Composable
fun LanguageDialog(
    current: String,
    onPick: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var staged by remember(current) { mutableStateOf(current) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { DialogTitleWithClose(stringResource(R.string.chat_language), onDismiss) },
        text = {
            LanguageWheelPicker(
                options = LANGUAGE_OPTIONS,
                selected = staged,
                onSelectedChange = { staged = it },
            )
        },
        confirmButton = {
            TextButton(onClick = { onPick(staged) }) {
                Text(stringResource(R.string.common_done))
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
private fun LanguageWheelPicker(
    options: List<Pair<String, String>>,
    selected: String,
    onSelectedChange: (String) -> Unit,
) {
    val itemHeight = 44.dp
    val visibleCount = 5  // odd: middle slot is the selected one
    val sideCount = visibleCount / 2
    val pickerHeight = itemHeight * visibleCount

    val initialIndex = options.indexOfFirst { it.first == selected }.coerceAtLeast(0)
    val listState = rememberLazyListState(initialFirstVisibleItemIndex = initialIndex)
    val flingBehavior = rememberSnapFlingBehavior(listState)
    val scope = rememberCoroutineScope()

    val centeredIndex by remember(options) {
        derivedStateOf {
            val info = listState.layoutInfo
            if (info.visibleItemsInfo.isEmpty()) initialIndex
            else {
                val center = (info.viewportStartOffset + info.viewportEndOffset) / 2
                info.visibleItemsInfo.minBy { abs(it.offset + it.size / 2 - center) }.index
            }
        }
    }

    LaunchedEffect(centeredIndex, listState.isScrollInProgress) {
        if (!listState.isScrollInProgress) {
            val code = options.getOrNull(centeredIndex)?.first
            if (code != null && code != selected) onSelectedChange(code)
        }
    }

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(pickerHeight),
    ) {
        LazyColumn(
            state = listState,
            flingBehavior = flingBehavior,
            contentPadding = PaddingValues(vertical = itemHeight * sideCount),
            modifier = Modifier.fillMaxSize(),
        ) {
            itemsIndexed(options) { index, (_, label) ->
                val distance = abs(index - centeredIndex).coerceAtMost(sideCount)
                val alpha = 1f - distance * 0.3f
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(itemHeight)
                        .clickable { scope.launch { listState.animateScrollToItem(index) } },
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        text = label,
                        style = if (distance == 0) MaterialTheme.typography.titleMedium
                                else MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface.copy(alpha = alpha),
                    )
                }
            }
        }
        val bandColor = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.6f)
        HorizontalDivider(
            color = bandColor,
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(top = itemHeight * sideCount),
        )
        HorizontalDivider(
            color = bandColor,
            modifier = Modifier
                .align(Alignment.TopCenter)
                .padding(top = itemHeight * (sideCount + 1)),
        )
    }
}

// The picker takes plain parameters and reads no AppContainer, so it renders in
// the IDE as-is. See ui/DrawerRow.kt for why previews exist at all here.
@Preview(name = "Language dialog", showBackground = true, heightDp = 420)
@Composable
private fun LanguageDialogPreview() {
    MirobodyTheme {
        LanguageDialog(current = "zh", onPick = {}, onDismiss = {})
    }
}
