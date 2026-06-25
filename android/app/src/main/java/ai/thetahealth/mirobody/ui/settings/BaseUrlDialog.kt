package ai.thetahealth.mirobody.ui.settings

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.ProvideLocale
import kotlinx.coroutines.launch
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BaseUrlDialog(
    currentLanguage: String,
    onDismiss: () -> Unit,
) {
    val container = LocalAppContainer.current
    val savedBaseUrl by container.settings.baseUrl.collectAsState(initial = null)
    val scope = rememberCoroutineScope()
    var input by remember(savedBaseUrl) { mutableStateOf(savedBaseUrl.orEmpty()) }
    var error by remember { mutableStateOf<String?>(null) }
    var presetsExpanded by remember { mutableStateOf(false) }
    val invalidMsg = stringResource(R.string.baseurl_invalid)

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            ProvideLocale(currentLanguage) {
                Text(stringResource(R.string.baseurl_title))
            }
        },
        text = {
            ProvideLocale(currentLanguage) {
                Column {
                    Text(
                        text = stringResource(R.string.baseurl_subtitle),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.height(12.dp))
                    ExposedDropdownMenuBox(
                        expanded = presetsExpanded,
                        onExpandedChange = { presetsExpanded = it },
                    ) {
                        OutlinedTextField(
                            value = input,
                            onValueChange = {
                                input = it
                                error = null
                                presetsExpanded = false
                            },
                            placeholder = { Text("http://10.0.2.2:18080") },
                            singleLine = true,
                            isError = error != null,
                            supportingText = error?.let { { Text(it) } },
                            trailingIcon = {
                                ExposedDropdownMenuDefaults.TrailingIcon(expanded = presetsExpanded)
                            },
                            modifier = Modifier
                                .fillMaxWidth()
                                .menuAnchor(MenuAnchorType.PrimaryEditable, enabled = true),
                            shape = RoundedCornerShape(10.dp),
                            colors = OutlinedTextFieldDefaults.colors(
                                focusedBorderColor = MaterialTheme.colorScheme.primary,
                                unfocusedBorderColor = MaterialTheme.colorScheme.outlineVariant,
                            ),
                        )
                        ExposedDropdownMenu(
                            expanded = presetsExpanded,
                            onDismissRequest = { presetsExpanded = false },
                        ) {
                            BASE_URL_PRESETS.forEach { preset ->
                                DropdownMenuItem(
                                    text = { Text(preset) },
                                    onClick = {
                                        input = preset
                                        error = null
                                        presetsExpanded = false
                                    },
                                )
                            }
                        }
                    }
                    Spacer(Modifier.height(8.dp))
                    Text(
                        text = stringResource(R.string.baseurl_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
        confirmButton = {
            ProvideLocale(currentLanguage) {
                TextButton(onClick = {
                    val trimmed = input.trim().trimEnd('/')
                    if (trimmed.toHttpUrlOrNull() == null) {
                        error = invalidMsg
                    } else {
                        scope.launch {
                            container.settings.setBaseUrl(trimmed)
                            onDismiss()
                        }
                    }
                }) {
                    Text(stringResource(R.string.common_save))
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