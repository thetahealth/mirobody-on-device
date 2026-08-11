package ai.thetahealth.mirobody.ui.health

import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.DialogTitleWithClose
import ai.thetahealth.mirobody.ui.LocalAppContainer
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.health.connect.client.PermissionController
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory

/**
 * Settings dialog that reads on-device health data and POSTs it as FHIR. It wires
 * the Health Connect permission launcher (the request needs an Activity result, so
 * it lives in the UI, not the repository); on Huawei/HMS or when no source is
 * present it degrades to a clear message.
 */
@Composable
fun HealthSyncDialog(
    onDismiss: () -> Unit,
) {
    val container = LocalAppContainer.current
    val vm: HealthViewModel = viewModel(
        factory = viewModelFactory {
            initializer { HealthViewModel(container.healthRepository, container.errorBus) }
        },
    )
    val state by vm.state.collectAsState()

    // On a granted result (or a denial) we just attempt the sync; the repository
    // reports "permissions not granted" if the user declined.
    val permissionLauncher = rememberLauncherForActivityResult(
        PermissionController.createRequestPermissionResultContract(),
    ) { _ -> vm.sync() }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { DialogTitleWithClose(stringResource(R.string.chat_sync_health), onDismiss) },
        text = {
            Column {
                val status = when {
                    state.loading -> stringResource(R.string.health_checking)
                    state.sourceName == null -> stringResource(R.string.health_no_source)
                    else -> stringResource(R.string.health_source, state.sourceName!!)
                }
                Text(status)
                state.result?.let { r ->
                    val msg = if (r.error != null) {
                        stringResource(R.string.health_error, r.error!!)
                    } else {
                        stringResource(R.string.health_result, r.posted, r.failed)
                    }
                    Text(msg, modifier = Modifier.padding(top = 8.dp))
                }
            }
        },
        confirmButton = {
            TextButton(
                enabled = !state.syncing && state.sourceName != null,
                onClick = {
                    if (state.permissions.isNotEmpty()) {
                        permissionLauncher.launch(state.permissions)
                    } else {
                        vm.sync()
                    }
                },
            ) {
                Text(
                    if (state.syncing) stringResource(R.string.health_syncing)
                    else stringResource(R.string.health_sync_button),
                )
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_cancel)) }
        },
    )
}
