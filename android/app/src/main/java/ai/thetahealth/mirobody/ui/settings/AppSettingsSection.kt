package ai.thetahealth.mirobody.ui.settings

import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.DrawerRow
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.LocalFontSizePreview
import kotlinx.coroutines.launch
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

/**
 * The app-settings group of the nav drawer: Language, Font size, Backend — each a
 * drawer row carrying its current value as a right-aligned hint, and each opening the
 * dialog that changes it.
 *
 * This used to be a top-right gear dropdown on both the chat and login screens. It
 * moved into the drawer to follow the web client (`htdoc/src/history.js`), which
 * folded the same menu in so every screen has exactly one menu affordance instead of
 * a hamburger and a gear competing for the same job. The login screen shows this
 * group on its own — everything else in the drawer is session-scoped.
 */
@Composable
internal fun AppSettingsSection(currentLanguage: String) {
    val container = LocalAppContainer.current
    val scope = rememberCoroutineScope()
    val fontOffset by container.settings.fontSizeOffset.collectAsState(initial = 0)
    val baseUrl by container.settings.baseUrl.collectAsState(initial = null)
    val fontSizePreview = LocalFontSizePreview.current
    var showLanguageDialog by remember { mutableStateOf(false) }
    var showFontSizeDialog by remember { mutableStateOf(false) }
    var showBackendDialog by remember { mutableStateOf(false) }

    DrawerRow(
        label = stringResource(R.string.chat_language),
        // The language's own endonym ("中文", "Français"), as the picker lists it —
        // a row reading "Language  English" is answerable without opening it.
        hint = LANGUAGE_OPTIONS.firstOrNull { it.first == currentLanguage }?.second
            ?: currentLanguage,
        onClick = { showLanguageDialog = true },
    )
    DrawerRow(
        label = stringResource(R.string.chat_font_size),
        hint = stringResource(fontTierLabelRes(fontOffset)),
        onClick = { showFontSizeDialog = true },
    )
    DrawerRow(
        label = stringResource(R.string.chat_backend),
        // Hostname only: the full address (scheme, port, path) belongs in the dialog
        // this row opens, not in a one-line hint that would ellipsize away the part
        // that identifies the server.
        hint = baseUrl?.toHttpUrlOrNull()?.host.orEmpty(),
        onClick = { showBackendDialog = true },
    )

    if (showLanguageDialog) {
        LanguageDialog(
            current = currentLanguage,
            onPick = { code ->
                scope.launch { container.settings.setLanguage(code) }
                showLanguageDialog = false
            },
            onDismiss = { showLanguageDialog = false },
        )
    }
    if (showFontSizeDialog) {
        FontSizeDialog(
            current = fontOffset,
            onPreview = { offset -> fontSizePreview.value = offset },
            onPick = { offset ->
                scope.launch { container.settings.setFontSizeOffset(offset) }
                showFontSizeDialog = false
            },
            onDismiss = {
                fontSizePreview.value = null
                showFontSizeDialog = false
            },
        )
    }
    if (showBackendDialog) {
        BaseUrlDialog(onDismiss = { showBackendDialog = false })
    }
}
