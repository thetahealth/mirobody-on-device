package ai.thetahealth.mirobody.ui.health

import ai.thetahealth.mirobody.R
import ai.thetahealth.mirobody.ui.DialogTitleWithClose
import ai.thetahealth.mirobody.ui.LocalAppContainer
import android.Manifest
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory

/**
 * Settings dialog for direct Bluetooth (BLE GATT) health sensors: scan, connect, and
 * stream a standard-profile sensor's readings into the FHIR store via
 * [ai.thetahealth.mirobody.data.health.ble.BleHealthController]. The Android sibling
 * of the desktop Qt BleDialog. Requests the runtime BLE permissions (BLUETOOTH_SCAN /
 * CONNECT on API 31+, ACCESS_FINE_LOCATION below) before scanning.
 */
@Composable
fun BleDeviceDialog(
    onDismiss: () -> Unit,
) {
    val container = LocalAppContainer.current
    val vm: BleViewModel = viewModel(
        factory = viewModelFactory {
            initializer { BleViewModel(container.bleHealthController) }
        },
    )
    val state by vm.state.collectAsState()

    val permissions = remember {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }
    // Launching with an already-granted set still invokes the callback, so this
    // doubles as the "start scan" action.
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { grants -> if (grants.values.all { it }) vm.startScan() }

    AlertDialog(
        onDismissRequest = { vm.disconnect(); onDismiss() },
        title = { DialogTitleWithClose(stringResource(R.string.ble_title), { vm.disconnect(); onDismiss() }) },
        text = {
            Column {
                Text(
                    stringResource(R.string.ble_intro),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                LazyColumn(modifier = Modifier.fillMaxWidth().heightIn(max = 220.dp).padding(top = 8.dp)) {
                    items(state.devices, key = { it.address }) { d ->
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable(enabled = !state.connected) { vm.connect(d.address) }
                                .padding(vertical = 6.dp),
                        ) {
                            Column(modifier = Modifier.weight(1f)) {
                                Text(d.name, style = MaterialTheme.typography.bodyMedium)
                                Text(
                                    d.address,
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                            if (d.supported) {
                                Text(
                                    stringResource(R.string.ble_supported_tag),
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.primary,
                                )
                            }
                        }
                    }
                }
                if (state.devices.isEmpty()) {
                    Text(
                        stringResource(R.string.ble_no_devices),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }

                if (state.status.isNotBlank()) {
                    Text(state.status, modifier = Modifier.padding(top = 8.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                state.lastReading?.let {
                    Text(it, modifier = Modifier.padding(top = 4.dp),
                        style = MaterialTheme.typography.titleSmall)
                }
                if (state.posted > 0 || state.failed > 0) {
                    Text(
                        stringResource(R.string.ble_saved, state.posted, state.failed),
                        modifier = Modifier.padding(top = 4.dp),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (state.connected) {
                    TextButton(onClick = { vm.disconnect() }, modifier = Modifier.padding(top = 4.dp)) {
                        Text(stringResource(R.string.ble_disconnect))
                    }
                }
            }
        },
        confirmButton = {
            TextButton(
                enabled = !state.connected,
                onClick = {
                    if (state.scanning) vm.stopScan() else permissionLauncher.launch(permissions)
                },
            ) {
                Text(
                    if (state.scanning) stringResource(R.string.ble_stop)
                    else stringResource(R.string.ble_scan),
                )
            }
        },
        dismissButton = {
            TextButton(onClick = { vm.disconnect(); onDismiss() }) {
                Text(stringResource(R.string.common_cancel))
            }
        },
    )
}
