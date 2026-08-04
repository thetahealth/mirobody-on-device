package ai.thetahealth.mirobody.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.theme.MirobodyTheme
import kotlin.math.roundToInt

/**
 * The five font tiers, persisted sp offset -> label. Top-level rather than local to
 * the dialog because the drawer row that opens it shows the current tier as its hint,
 * so both need the same offset->label mapping.
 */
internal val FONT_TIERS: List<Pair<Int, Int>> = listOf(
    -4 to R.string.chat_font_size_smaller,
    -2 to R.string.chat_font_size_small,
    0 to R.string.chat_font_size_normal,
    2 to R.string.chat_font_size_large,
    4 to R.string.chat_font_size_larger,
)

/** Label for a stored offset; anything unrecognized reads as the normal tier. */
internal fun fontTierLabelRes(offset: Int): Int =
    FONT_TIERS.firstOrNull { it.first == offset }?.second ?: R.string.chat_font_size_normal

/**
 * Font-size preference picker (a 5-tier slider). [onPreview] live-updates the
 * app's font scale while dragging; [onPick] persists the chosen tier. Shared by the
 * chat settings menu and the pre-auth login menu.
 */
@Composable
fun FontSizeDialog(
    current: Int,
    onPreview: (Int) -> Unit,
    onPick: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val tiers = FONT_TIERS
    val initialIndex = tiers.indexOfFirst { it.first == current }.let {
        if (it < 0) 2 else it
    }
    var stagedIndex by remember(current) { mutableStateOf(initialIndex) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.chat_font_size)) },
        text = {
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
        },
        confirmButton = {
            TextButton(onClick = { onPick(tiers[stagedIndex].first) }) {
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

@Preview(name = "Font size dialog", showBackground = true, heightDp = 320)
@Composable
private fun FontSizeDialogPreview() {
    MirobodyTheme {
        FontSizeDialog(current = 0, onPreview = {}, onPick = {}, onDismiss = {})
    }
}
