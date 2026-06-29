package ai.thetahealth.mirobody.ui.circle

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ArrowDropDown
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.LocalAppContainer
import ai.thetahealth.mirobody.ui.ProvideLocale
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory

/**
 * Share the current conversation with care-circle members (view / edit), or
 * unshare. The Android counterpart of the web client's share modal. Selections are
 * staged locally and committed on Save.
 */
@Composable
fun ShareConversationDialog(
    conversationId: String,
    currentLanguage: String,
    onDismiss: () -> Unit,
) {
    val container = LocalAppContainer.current
    val vm: ShareConversationViewModel = viewModel(
        factory = viewModelFactory {
            initializer { ShareConversationViewModel(container.circleRepository, container.errorBus) }
        },
    )
    LaunchedEffect(conversationId) { vm.load(conversationId) }
    val state by vm.state.collectAsState()

    // Local editable selection per member, seeded once targets load.
    val checked = remember { mutableStateMapOf<Long, Boolean>() }
    val access = remember { mutableStateMapOf<Long, String>() }
    LaunchedEffect(state.targets) {
        state.targets.forEach { t ->
            if (t.member !in checked) checked[t.member] = t.wasShared
            if (t.member !in access) access[t.member] = t.initialAccess
        }
    }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = {
            ProvideLocale(currentLanguage) { Text(stringResource(R.string.share_title)) }
        },
        text = {
            ProvideLocale(currentLanguage) {
                when {
                    state.loading -> Box(
                        modifier = Modifier.fillMaxWidth(),
                        contentAlignment = Alignment.Center,
                    ) { CircularProgressIndicator() }
                    state.targets.isEmpty() -> Text(
                        stringResource(R.string.share_no_members),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    else -> Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(max = 360.dp)
                            .verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(2.dp),
                    ) {
                        state.targets.forEach { t ->
                            val isChecked = checked[t.member] ?: false
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Checkbox(
                                    checked = isChecked,
                                    onCheckedChange = { checked[t.member] = it },
                                )
                                Text(
                                    text = t.nickname.ifBlank { t.email.ifBlank { "#${t.member}" } },
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurface,
                                    maxLines = 1,
                                    modifier = Modifier.weight(1f),
                                )
                                AccessDropdown(
                                    value = access[t.member] ?: "view",
                                    enabled = isChecked,
                                    onChange = { access[t.member] = it },
                                )
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            ProvideLocale(currentLanguage) {
                if (state.targets.isNotEmpty()) {
                    TextButton(
                        enabled = !state.busy,
                        onClick = {
                            val sels = state.targets.map { t ->
                                ShareSelection(
                                    member = t.member,
                                    checked = checked[t.member] ?: false,
                                    access = access[t.member] ?: "view",
                                    wasShared = t.wasShared,
                                )
                            }
                            vm.apply(conversationId, sels, onDismiss)
                        },
                    ) { Text(stringResource(R.string.share_save)) }
                }
            }
        },
        dismissButton = {
            ProvideLocale(currentLanguage) {
                TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_cancel)) }
            }
        },
    )
}

@Composable
private fun AccessDropdown(
    value: String,
    enabled: Boolean,
    onChange: (String) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    val labelRes = if (value == "edit") R.string.share_access_edit else R.string.share_access_view
    TextButton(onClick = { expanded = true }, enabled = enabled) {
        Text(stringResource(labelRes), style = MaterialTheme.typography.bodySmall)
        Icon(Icons.Outlined.ArrowDropDown, contentDescription = null)
    }
    DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
        DropdownMenuItem(
            text = { Text(stringResource(R.string.share_access_view)) },
            onClick = { onChange("view"); expanded = false },
        )
        DropdownMenuItem(
            text = { Text(stringResource(R.string.share_access_edit)) },
            onClick = { onChange("edit"); expanded = false },
        )
    }
}
