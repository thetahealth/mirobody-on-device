package ai.thetahealth.mirobody.ui.health

import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.DialogTitleWithClose
import ai.thetahealth.mirobody.ui.LocalAppContainer
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
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory

/**
 * Settings dialog for the legacy classic-Bluetooth **HDP** path (IEEE 11073), shown only
 * on Android 9 and below. It registers a health sink and waits for a device to connect;
 * incoming measurements are POSTed as FHIR. Experimental — see [HdpHealthController].
 */
@Composable
fun HdpDeviceDialog(
    onDismiss: () -> Unit,
) {
    val container = LocalAppContainer.current
    val vm: HdpViewModel = viewModel(
        factory = viewModelFactory {
            initializer { HdpViewModel(container.hdpHealthController) }
        },
    )
    val state by vm.state.collectAsState()

    AlertDialog(
        onDismissRequest = { vm.stop(); onDismiss() },
        title = { DialogTitleWithClose(stringResource(R.string.hdp_title), { vm.stop(); onDismiss() }) },
        text = {
            Column {
                Text(
                    if (state.supported) stringResource(R.string.hdp_intro)
                    else stringResource(R.string.hdp_unsupported),
                )
                if (state.status.isNotBlank()) {
                    Text(state.status, modifier = Modifier.padding(top = 8.dp))
                }
                state.connectedDevice?.let {
                    Text(it, modifier = Modifier.padding(top = 4.dp))
                }
                state.lastReading?.let {
                    Text(it, modifier = Modifier.padding(top = 4.dp))
                }
                if (state.posted > 0 || state.failed > 0) {
                    Text(
                        stringResource(R.string.ble_saved, state.posted, state.failed),
                        modifier = Modifier.padding(top = 4.dp),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                enabled = state.supported,
                onClick = { if (state.listening) vm.stop() else vm.start() },
            ) {
                Text(
                    if (state.listening) stringResource(R.string.hdp_stop)
                    else stringResource(R.string.hdp_start),
                )
            }
        },
        dismissButton = {
            TextButton(onClick = { vm.stop(); onDismiss() }) {
                Text(stringResource(R.string.common_cancel))
            }
        },
    )
}
