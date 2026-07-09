package ai.thetahealth.mirobody.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.LocalFontSizePreview
import kotlinx.coroutines.launch

/**
 * The app settings menu, shared by the login and chat screens so settings sit in
 * one consistent place across both (mirrors the web client's top-right gear).
 * A gear icon opening a dropdown: Language, Font size, Backend, About. Everything
 * session-scoped (history, health connections, incognito, sign out) lives in the
 * chat screen's left nav drawer instead.
 */
@Composable
internal fun AppSettingsMenu(currentLanguage: String) {
    val container = LocalAppContainer.current
    val scope = rememberCoroutineScope()
    val fontOffset by container.settings.fontSizeOffset.collectAsState(initial = 0)
    val fontSizePreview = LocalFontSizePreview.current
    var expanded by remember { mutableStateOf(false) }
    var showLanguageDialog by remember { mutableStateOf(false) }
    var showFontSizeDialog by remember { mutableStateOf(false) }
    var showBackendDialog by remember { mutableStateOf(false) }
    var showAboutDialog by remember { mutableStateOf(false) }
    IconButton(onClick = { expanded = true }) {
        Icon(
            Icons.Outlined.Settings,
            contentDescription = stringResource(R.string.common_settings),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
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
            text = { Text(stringResource(R.string.chat_about)) },
            onClick = {
                expanded = false
                showAboutDialog = true
            },
        )
    }
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
    if (showAboutDialog) {
        AboutDialog(onDismiss = { showAboutDialog = false })
    }
}

/** Small dialog with the app name + build version (the gear's About item). */
@Composable
private fun AboutDialog(onDismiss: () -> Unit) {
    val context = LocalContext.current
    val versionName = remember(context) {
        runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName
        }.getOrNull().orEmpty()
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.chat_about)) },
        text = {
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
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.common_close))
            }
        },
    )
}
