package ai.thetahealth.mirobody.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.KeyboardArrowLeft
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import android.content.res.Configuration
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.theme.MirobodyTheme

/**
 * One tappable row in the nav drawer: optional leading icon, label, optional
 * right-aligned hint (the current value — e.g. the chosen language or the backend
 * host). `danger` paints the label red (Sign out).
 *
 * Lives here rather than beside the chat drawer because the app-settings group
 * (ui/settings/AppSettingsSection.kt) renders the same row and is shown from both the
 * chat drawer and the login drawer.
 */
@Composable
internal fun DrawerRow(
    label: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    hint: String? = null,
    leading: (@Composable () -> Unit)? = null,
    danger: Boolean = false,
    center: Boolean = false,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 20.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = if (center) Arrangement.Center else Arrangement.Start,
    ) {
        if (leading != null) {
            leading()
            Spacer(Modifier.width(10.dp))
        }
        Text(
            text = label,
            style = MaterialTheme.typography.bodyLarge,
            color = if (danger) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
        )
        if (hint != null) {
            Spacer(Modifier.weight(1f))
            Text(
                text = hint,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(start = 12.dp),
            )
        }
    }
}

/** The hairline that separates the drawer's pinned groups. */
@Composable
internal fun DrawerDivider() {
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f))
}

/**
 * The drawer's header: a back button that closes it, then the "Menu" title. Shared by
 * the chat drawer and the login screen's settings-only drawer so the two open into the
 * same frame.
 */
@Composable
internal fun DrawerHeader(onClose: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 4.dp, end = 12.dp, top = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        IconButton(onClick = onClose) {
            // A chevron, not an arrow — Harmony's sys.symbol.chevron_left. This
            // dismisses a panel; it does not navigate back through a history, and the
            // heavier arrow claims it does. Auto-mirrored so RTL flips it.
            Icon(
                Icons.AutoMirrored.Outlined.KeyboardArrowLeft,
                contentDescription = stringResource(R.string.common_back),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Text(
            text = stringResource(R.string.chat_menu_title),
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}

// ---------------------------------------------------------------------------
// Compose previews. There is no layout XML in this app, so the Design pane only
// has something to show where a @Preview exists — these are the drawer's shared
// pieces. Everything here is a pure composable (no AppContainer, no ViewModel,
// no network), which is what makes it renderable in the IDE at all; screens that
// read LocalAppContainer cannot be previewed without a fake container.
// ---------------------------------------------------------------------------

// Labels are literals rather than stringResource lookups so a preview renders the
// same regardless of which locale the IDE picks.
@Preview(name = "Drawer pieces · light", showBackground = true)
@Preview(name = "Drawer pieces · dark", showBackground = true, uiMode = Configuration.UI_MODE_NIGHT_YES)
@Composable
private fun DrawerPiecesPreview() {
    MirobodyTheme {
        Surface(color = MaterialTheme.colorScheme.background) {
            Column {
                DrawerHeader(onClose = {})
                DrawerDivider()
                // Hint-less rows, as the health group renders them.
                DrawerRow(label = "Care circle", onClick = {})
                DrawerRow(label = "Connect EHR", onClick = {})
                DrawerDivider()
                // Rows carrying their current value — the app-settings group.
                DrawerRow(label = "Language", onClick = {}, hint = "English")
                DrawerRow(label = "Font size", onClick = {}, hint = "Normal")
                DrawerRow(label = "Backend", onClick = {}, hint = "test.mirobody.ai")
                DrawerDivider()
                DrawerRow(label = "Sign out", onClick = {}, danger = true, center = true)
            }
        }
    }
}
